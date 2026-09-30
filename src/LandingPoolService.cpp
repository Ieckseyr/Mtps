// LandingPoolService.cpp - 预落点池的开服装配与后台补齐（见 LandingPoolService.h）
#include "LandingPoolService.h"

#include "ArchiveScanner.h"
#include "BiomeSampler.h"
#include "Config.h"
#include "PreLandingPool.h"
#include "RandomTeleportInternal.h"   // RTP_LANDING_DEFER_SECONDS

#include <ll/api/Versions.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/Bedrock.h>

#include <mc/world/level/Level.h>
#include <mc/world/level/LevelSeed64.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace mtps {

namespace {

ll::io::Logger& log() { return ll::mod::NativeMod::current()->getLogger(); }

std::unique_ptr<std::thread> sThread;
std::atomic<bool>            sStop{false};

// BDS 版本三元组检测：ll::getGameVersion 优先，进程 exe 的 FileVersion 资源后备。
// 与 ZXDash（plugins/ZXPanel/src/mod/ZXPanel.cpp::detectBiomeMC）同一套做法，
// 保证两边算出同一个 cubiomes 算法版本。
void detectVersion(int& major, int& minor, int& patch) {
    major = minor = patch = 0;
    try {
        auto v = ll::getGameVersion();
        major = v.major;
        minor = v.minor;
        patch = v.patch;
    } catch (...) {}
    if (major > 0) return;
#ifdef _WIN32
    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        DWORD handle = 0;
        DWORD sz     = GetFileVersionInfoSizeA(exePath, &handle);
        if (sz > 0 && sz < (1u << 20)) {
            std::vector<char> buf(sz);
            if (GetFileVersionInfoA(exePath, 0, sz, buf.data())) {
                VS_FIXEDFILEINFO* fi    = nullptr;
                UINT              fiLen = 0;
                if (VerQueryValueA(buf.data(), "\\", reinterpret_cast<LPVOID*>(&fi), &fiLen) && fi
                    && fiLen >= sizeof(VS_FIXEDFILEINFO)) {
                    major = (fi->dwFileVersionMS >> 16) & 0xFFFF;
                    minor = fi->dwFileVersionMS & 0xFFFF;
                    patch = (fi->dwFileVersionLS >> 16) & 0xFFFF;
                }
            }
        }
    }
#endif
}

// 可中断的等待（关服时每 100ms 看一眼，不会把 join 卡住）
void waitTicks(std::atomic<bool>& stop, int tenths) {
    for (int i = 0; i < tenths; i++) {
        if (stop.load(std::memory_order_acquire)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void runTopUp() {
    auto& cfg  = Config::getInstance();
    auto& pool = PreLandingPool::getInstance();

    // ① 等世界与种子。两个坑：极早期 Level 还没建（getLevel 为空）；Level 在了但世界还没加载完时
    //    种子读到 0，拿 0 配采样器会让整场会话的群系判断全错，所以先等非 0（上限 60 秒）。
    int64_t seed    = 0;
    bool    gotSeed = false;
    for (int i = 0; i < 600 && !sStop.load(std::memory_order_acquire); i++) {
        auto lvl = ll::service::getLevel();
        if (lvl) {
            try {
                seed    = (int64_t)lvl->getLevelSeed64().mValue;
                gotSeed = true;
                if (seed != 0) break;
            } catch (...) {}
        }
        waitTicks(sStop, 10);
    }
    if (!gotSeed || sStop.load(std::memory_order_acquire)) {
        log().warn("[RTP][预落点池] 未取到世界种子, 本轮跳过建池（随机传送走原有降级路径）");
        return;
    }
    if (seed == 0) {
        log().warn("[RTP][预落点池] 等 60 秒后世界种子仍为 0（世界可能还没加载完）—— "
                   "按 0 继续; 若群系判定看起来不对, 重启一次即可");
    }

    // ② 版本检测 → cubiomes 算法版本（与 ZXDash 群系底图同一映射）
    int         major = 0, minor = 0, patch = 0;
    std::string ver;
    detectVersion(major, minor, patch);
    int const   mc = BiomeSampler::mcForVersion(major, minor, patch, &ver);

    if (!cfg.landingPoolEnabled()) {
        log().info("[RTP][预落点池] 已关闭（randomTeleport.pool.enabled=false）→ 随机传送走原有四级数据源");
        return;
    }

    // ③ 配置群系采样器（群系排除 + 邻域口径 + 底图同切片）
    bool const samplerOk = BiomeSampler::getInstance().configure(
        seed, mc, cfg.biomeExcludeBiomes(), cfg.biomeNeighborRadius());
    if (!samplerOk) {
        log().warn("[RTP][预落点池] 群系采样器配置失败（cubiomes 版本 {} 非法）, 池不启用, 随机传送走降级",
                   mc);
        return;
    }
    log().info("[RTP][预落点池] 世界种子 {}（BDS {} → cubiomes MC{}）, {}", (long long)seed,
               ver.empty() ? "?" : ver, mc, BiomeSampler::getInstance().diagInfo());

    // ④ 载入池（指纹不符 → 作废重建）+ 同步区域
    pool.setSeedInfo(seed, mc);
    {
        PoolOptions o;
        o.enabled              = cfg.landingPoolEnabled();
        o.loadedTarget         = cfg.landingPoolLoadedTarget();
        o.unloadedTarget       = cfg.landingPoolUnloadedTarget();
        o.bucketSize           = cfg.landingPoolBucketSize();
        o.minSeparation        = cfg.landingPoolMinSeparation();
        o.maxTriesPerPoint     = cfg.landingPoolMaxTries();
        o.approxHeightMin      = cfg.landingPoolApproxHeightMin();
        o.replaceRatio         = cfg.landingPoolReplaceRatio();
        o.rebuildOnRadiusChange = cfg.landingPoolRebuildOnRadiusChange();
        o.file                 = cfg.landingPoolFile();
        pool.setOptions(o);
    }
    bool const cacheHit = pool.load();
    log().info("[RTP][预落点池] {}（{}）", cacheHit ? "已载入落盘池" : "无可用落盘池, 将全量建池",
               pool.diagInfo());
    syncPoolRegionsFromPresets();

    // 逐维度补齐缺口（只补缺 —— 这就是"再开服不乱扫"的落地）。
    // archiveReady=false 时落点表还没就绪 → 只建未加载类（那批点来自 cubiomes, 与存档无关）。
    auto topUpAll = [&](bool archiveReady) {
        auto const t0 = std::chrono::steady_clock::now();
        for (int dim = 0; dim <= 2 && !sStop.load(std::memory_order_acquire); dim++) {
            auto const reg = pool.regionOf(dim);
            if (!reg.valid) continue;   // 这个维度没有启用预设 → 不需要池

            auto const r    = pool.topUp(dim);
            auto const opts = pool.options();
            log().info("[RTP][预落点池] dim{} 半径 {} 起点 ({:.0f},{:.0f}): 本次补充 已加载 {} / 未加载 {}"
                       "（替换旧点 {} 区域外清理 {} 超目标淘汰 {} 失效/过时清理 {}）; 库存 已加载 {}/{} 未加载 {}/{}",
                       dim, reg.radius, reg.originX, reg.originZ, r.loadedAdded, r.unloadedAdded,
                       r.unloadedReplaced, r.outOfRegion, r.evicted, r.dropped,
                       pool.count(dim, kPlTierLoaded, true), opts.loadedTarget,
                       pool.count(dim, kPlTierUnloaded, true), (int)r.effectiveUnloadedTarget);
            if (archiveReady && r.loadedAdded < (size_t)opts.loadedTarget) {
                // 目标没配满时把原因说清楚：存档里在区域内、群系合格、且池里还没有的落点就这么多
                log().info("[RTP][预落点池] dim{} 已加载类未配满 {}: 考察存档落点 {} 个（已在池中 {} / "
                           "新合格 {}），目标 {} —— 想配满得缩小半径或让玩家多探索; 未加载类不受影响",
                           dim, pool.count(dim, kPlTierLoaded, true), r.loadedStats.examined,
                           r.loadedStats.already, r.loadedStats.qualified, opts.loadedTarget);
            }
            if (!archiveReady) {
                log().info("[RTP][预落点池] dim{} 落点表还没就绪 → 本轮先只建未加载类"
                           "（已加载类等落点表好了再补；未加载类不依赖存档）", dim);
            }
        }
        auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        log().info("[RTP][预落点池] 补齐完成（{}ms）: {}", ms, pool.diagInfo());
    };

    // ⑤ **第一轮补齐（不等落点表）**：未加载类那 3000 个点完全来自 cubiomes 推算, 与存档无关,
    //    没有理由等存档索引/重算完。先建起来, 这样开服十几秒后池就能用了 ——
    //    实测有人开服 43 秒传送、池还差 2 秒才建好, 于是退回"均匀随机点"撞上一块大洋,
    //    白生成 5.8 秒。早一轮就绪就不会有这种窗口。
    topUpAll(false);
    pool.save();

    // ⑥ 等落点表就绪（已加载类要从它取点）→ 第二轮补齐补上已加载类。
    //    要么超时放弃已加载类，未加载类照样能用。
    bool archiveReady = false;
    // 上限 35 分钟: 落点表重建会"等空服"最多 30 分钟，这里必须比它长，否则池的"已加载类"
    // 补齐会被饿死（未加载类早已就绪, 不受影响）。
    for (int i = 0; i < 21000 && !sStop.load(std::memory_order_acquire); i++) {
        if (ArchiveScanner::getInstance().landingsReady()) {
            archiveReady = true;
            break;
        }
        waitTicks(sStop, 10);
    }
    if (!archiveReady) {
        log().warn("[RTP][预落点池] 落点表尚未就绪（可能预计算被关或世界很大）, "
                   "已加载类留待下次补齐; 未加载类已就绪, 随机传送不受影响");
    } else {
        topUpAll(true);
    }
    pool.save();

    // ⑦ 自检：跑一批合成抽取，确认 pick() 真的能命中、落点在半径内、抖动在命中半径内。
    // 为什么每次都跑（而不是放在 debug 开关下）：pick() 一旦有问题（比如桶索引没建起来），
    // 表现是"静默永远走降级"——不报错、功能看起来正常、只是慢。只有把这个数字打出来才看得见。
    // 成本：每个维度 500 次抽取 ≈ 几十毫秒，且只在后台线程、开服后一次性。
    {
        for (int dim = 0; dim <= 2; dim++) {
            auto const reg = pool.regionOf(dim);
            if (!reg.valid) continue;
            int    hits = 0, tier0 = 0, tier1 = 0;
            double dsum = 0, dmin = 1e18, dmax = 0;
            constexpr int kTrials = 500;
            for (int i = 0; i < kTrials; i++) {
                PreLandingPool::Pick pk{};
                uint64_t const s = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1) + 12345;
                if (!pool.pick(dim, reg.originX, reg.originZ, reg.radius, s, 0, 0, 0, pk)) continue;
                hits++;
                if (pk.tier == kPlTierLoaded) tier0++; else tier1++;
                double const dx = pk.targetX - reg.originX, dz = pk.targetZ - reg.originZ;
                double const dist = std::sqrt(dx * dx + dz * dz);
                dsum += dist;
                if (dist < dmin) dmin = dist;
                if (dist > dmax) dmax = dist;
            }
            if (hits == 0) {
                log().warn("[RTP][预落点池] 自检 dim{}: {} 次抽取一次都没命中 —— 池不可用, "
                           "随机传送会一直走降级（检查 pool.enabled / 目标数量 / 半径）",
                           dim, kTrials);
                continue;
            }
            log().info("[RTP][预落点池] 自检 dim{}: {}/{} 命中 ({:.0f}%), 已加载类 {} / 未加载类 {}; "
                       "落点距原点 平均 {:.0f} 最小 {:.0f} 最大 {:.0f}（半径 {}, 应在半径内）",
                       dim, hits, kTrials, hits * 100.0 / kTrials, tier0, tier1, dsum / hits, dmin,
                       dmax, reg.radius);
        }
    }
}

// 池的起跑时机和落点表分开：落点表要扫 .ldb + 逐 chunk 解码（磁盘重活）所以延后 45 秒，
// 池只是读一个 100KB 的缓存 + cubiomes 纯 CPU 推算，而且未加载类那批点压根不依赖存档，
// 没理由陪它等（实测有人开服第 43 秒传送、池还差 2 秒没好，只好退回均匀随机点，白生成 5.8 秒）。
inline constexpr int RTP_POOL_DEFER_SECONDS = 5;

} // namespace

void startLandingPoolService() {
    if (sThread) return;   // 幂等
    if (!Config::getInstance().landingPoolEnabled()) {
        log().info("[RTP][预落点池] 已关闭（randomTeleport.pool.enabled=false）");
        return;
    }
    sStop.store(false, std::memory_order_release);
    sThread = std::make_unique<std::thread>([] {
        log().info("[RTP][预落点池] 已安排后台建池（{}s 后开始; 未加载类不等落点表, 只补缺口）",
                   RTP_POOL_DEFER_SECONDS);
        waitTicks(sStop, RTP_POOL_DEFER_SECONDS * 10);
        if (sStop.load(std::memory_order_acquire)) return;
        try {
            runTopUp();
        } catch (...) {
            log().warn("[RTP][预落点池] 建池过程抛出异常, 已放弃（随机传送走原有降级路径）");
        }
    });
}

void stopLandingPoolService() {
    sStop.store(true, std::memory_order_release);
    if (sThread && sThread->joinable()) sThread->join();
    sThread.reset();
}

void syncPoolRegionsFromPresets() {
    auto const& presets = Config::getInstance().randomPresets();
    struct Best { int radius{0}; double ox{0}, oz{0}; bool valid{false}; };
    Best best[3];
    for (auto const& p : presets) {
        if (!p.enabled) continue;
        if (p.dimid < 0 || p.dimid > 2) continue;
        if (p.radius <= 0) continue;
        if (!best[p.dimid].valid || p.radius > best[p.dimid].radius) {
            best[p.dimid] = {p.radius, p.originX, p.originZ, true};
        }
    }
    std::vector<PreLandingPool::Region> regions;
    regions.reserve(3);
    for (int i = 0; i < 3; i++) {
        if (best[i].valid) regions.push_back({i, best[i].ox, best[i].oz, best[i].radius, true});
    }
    PreLandingPool::getInstance().setRegions(regions);
}


} // namespace mtps
