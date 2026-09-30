// PoolWarmer.cpp - 预落点池的后台预热（见 PoolWarmer.h）
#include "PoolWarmer.h"

#include "Config.h"
#include "RandomTeleport.h"
#include "RandomTeleportInternal.h"   // isChunkReady / chunkStateName / scanChunkMemory / 区域封装

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/Bedrock.h>

#include <mc/world/level/Level.h>
#include <mc/world/level/ticking/AddTickingAreaStatus.h>
#include <mc/world/level/dimension/Dimension.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_set>

namespace mtps {

namespace {

ll::io::Logger& log() { return ll::mod::NativeMod::current()->getLogger(); }

// 预热等区块就绪的上限（tick）。正常 20~120 tick；超过判异常、放弃本次（不占着区域）。
constexpr int kWaitLimitTicks = 400;

} // namespace

PoolWarmer& PoolWarmer::getInstance() {
    static PoolWarmer instance;
    return instance;
}

void PoolWarmer::start() {
    if (mRunning) return;
    auto& cfg = Config::getInstance();
    if (!cfg.landingPoolWarmEnabled()) {
        log().info("[RTP][预热] 未启用（randomTeleport.pool.warm.enabled=false）");
        return;
    }
    mRunning  = true;
    mPhase    = 0;
    // 开服后先等一会，把开服负载让过去（区块生成是最贵的一件事）
    mCooldown = (int64_t)std::max(5, cfg.landingPoolWarmStartDelaySeconds()) * 20;
    log().info("[RTP][预热] 已启用: 常驻位不满时每 5 秒补一个预落点（一次生成 25 个区块）, "
               "满了之后回到 {} 秒的闲置节奏; 常驻 {} 个, {} 秒后开始{}",
               cfg.landingPoolWarmIntervalSeconds(), cfg.landingPoolWarmKeepResident(),
               cfg.landingPoolWarmStartDelaySeconds(),
               cfg.landingPoolWarmOnlyWhenIdle() ? ", 有玩家传送时让路" : "");
}

void PoolWarmer::stop() {
    if (!mRunning) return;
    mRunning = false;
    if (!mArea.empty()) {
        releaseArea(mArea, mDim);
        mArea.clear();
    }
    for (auto& s : mResident) releaseSpot(s);
    mResident.clear();
    refreshResidentList();
    log().info("[RTP][预热] 已停用（累计预热 {}: 验证通过 {} 无安全列 {} 放弃 {}）", mWarmed,
               mVerified, mNoSafe, mAborted);
}

void PoolWarmer::releaseArea(std::string const& name, int dim) {
    if (name.empty()) return;
    auto level = ll::service::getLevel();
    if (!level) return;   // 关服末期拿不到 Level: 残留由下次启动的前缀清理兜底
    removeRtpArea(*level, dim, name);
}

void PoolWarmer::releaseSpot(Spot& s) {
    if (s.area.empty()) return;
    releaseArea(s.area, s.dim);
    s.area.clear();
}

void PoolWarmer::refreshResidentList() {
    std::vector<PreLanding> pts;
    pts.reserve(mResident.size());
    for (auto const& s : mResident) {
        if (!s.area.empty()) pts.push_back(s.p);
    }
    PreLandingPool::getInstance().setResident(pts);
}

// 玩家用到过这个点 → 释放常驻位，位子留给下一个预热出来的点（原因见头文件）
void PoolWarmer::noteUsed(int dim, int x, int z) {
    bool changed = false;
    for (auto it = mResident.begin(); it != mResident.end();) {
        if (it->dim == dim && it->p.x == x && it->p.z == z) {
            RTP_DBG("[RTP][预热] 常驻点 ({}, {}) 已被使用 → 释放常驻位", x, z);
            releaseSpot(*it);
            it      = mResident.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) refreshResidentList();
}

void PoolWarmer::startWarm() {
    auto& cfg  = Config::getInstance();
    auto& pool = PreLandingPool::getInstance();
    if (!pool.usable()) return;   // 池还没建好（开服早期）

    auto level = ll::service::getLevel();
    if (!level) return;

    static int      sDimCursor = 0;
    std::vector<std::pair<int, int>> recent;
    recent.reserve(mResident.size());
    for (auto const& s : mResident) recent.push_back({s.p.x, s.p.z});

    static uint64_t sSeed = (uint64_t)std::time(nullptr);
    for (int attempt = 0; attempt < 3; attempt++) {
        int const  dim = sDimCursor % 3;
        sDimCursor     = (sDimCursor + 1) % 3;
        auto const reg = pool.regionOf(dim);
        if (!reg.valid) continue;

        sSeed = sSeed * 6364136223846793005ull + 1442695040888963407ull;
        PreLandingPool::Pick pk{};
        if (!pool.takeWarmCandidate(dim, reg.originX, reg.originZ, reg.radius, sSeed, recent, pk)) {
            continue;
        }
        mDim = dim;
        mX   = pk.x;
        mZ   = pk.z;
        if (attachWarmArea(*level)) return;
    }

    // 三个维度都没有可预热的点（都热过了 / 池没建好）→ 长冷却后再看
    mPhase    = 0;
    mCooldown = 60 * 20;
}

// 挂上预热用的常加载区域，成功就进入"等区块就绪"阶段
bool PoolWarmer::attachWarmArea(Level& level) {
    mArea = makeAreaName("warm");
    auto const st =
        addRtpArea(level, mDim, mArea, mX, mZ, RTP_WAIT_AREA_RADIUS_CHUNKS);
    if (st != ::AddTickingAreaStatus::Success) {
        mArea.clear();
        log().warn("[RTP][预热] 常加载区域登记失败(status={}) 点 ({}, {}) → 20 秒后重试", (int)st, mX,
                   mZ);
        mPhase    = 0;
        mCooldown = 20 * 20;
        return true;   // 已处理（不算"没挂上就往下走"）
    }
    mAreaCX    = mX;
    mAreaCZ    = mZ;
    mPhase     = 1;
    mWaitTicks = 0;
    RTP_DBG("[RTP][预热] 开始: dim{} 点 ({}, {}), 区域 r={}", mDim, mX, mZ,
            RTP_WAIT_AREA_RADIUS_CHUNKS);
    return true;
}

void PoolWarmer::finishWarm(bool ok, int realY) {
    auto& pool      = PreLandingPool::getInstance();
    int const dim   = mDim;
    int const x     = mX, z = mZ;
    int const ticks = mWaitTicks;

    mWarmed++;
    mLastTicks = ticks;
    if (ok) {
        pool.markVerified(dim, x, z, realY);   // 升级成"已加载类"：玩家再落到它就是"加载"而非"生成"
        mVerified++;
        mLastRealY = realY;
    } else {
        pool.markInvalid(dim, x, z);           // 整块无安全列 → 报废，后台补齐会补新点
        mNoSafe++;
    }

    // 常驻：本次的点留在内存里（抽到它就是瞬时传送）；超出 keepResident 的旧点撤掉
    int const keep = std::max(0, Config::getInstance().landingPoolWarmKeepResident());
    if (keep > 0 && ok) {
        Spot s;
        s.p             = PreLanding{};
        s.p.x           = x;
        s.p.z           = z;
        s.p.approxY     = realY;
        s.p.biome       = 0;
        s.p.tier        = kPlTierUnloaded;    // 存"预热前"的类别只是标记；"被用过"靠 noteUsed 显式通知
        s.p.state       = kPlStateCandidate;
        s.dim           = dim;
        s.area          = mArea;              // 区域留着不撤 → 区块保持加载
        mResident.push_back(s);
        while ((int)mResident.size() > keep) {
            releaseSpot(mResident.front());
            mResident.erase(mResident.begin());
        }
        refreshResidentList();
    } else {
        releaseArea(mArea, dim);              // 不常驻：撤区域（点已经"生成过"，传送只需加载）
    }
    mArea.clear();

    RTP_DBG("[RTP][预热] 完成 {} ({}, {}): 等 {} tick → {}", ok ? "✓" : "✗", x, z, ticks,
            ok ? ("验证通过 y=" + std::to_string(realY) + "，已升级为已加载类")
               : "整块无安全列，已报废");
    mPhase = 0;
    // 常驻位没满就隔 5 秒接着热，满了才回到 intervalSeconds 的慢节奏。
    // "补一个"的生成工作量和"玩家现场等引擎生成"是同一份工作，只是搬到后台做（还会给玩家让路），
    // 玩家那侧因此变成瞬时（实测 66ms/2tick），总工作量没变。
    mCooldown = ((int)mResident.size() < keep ? 5 : std::max(1, Config::getInstance().landingPoolWarmIntervalSeconds())) * 20;
}

void PoolWarmer::tick() {
    if (!mRunning) return;
    auto& cfg = Config::getInstance();
    if (!cfg.landingPoolWarmEnabled()) return;

    // 有玩家在传送就让路，不抢生成队列（也让 MSPT 更稳）
    if (cfg.landingPoolWarmOnlyWhenIdle() && RandomTeleport::getInstance().busy()) return;

    auto level = ll::service::getLevel();
    if (!level) return;

    if (mPhase == 0) {
        if (mCooldown > 0) {
            --mCooldown;
            return;
        }
        startWarm();
        return;
    }

    // 等区块就绪，再用与传送路径完全相同的规则扫一遍（口径不一致的话，热过的点传送时
    // 又会被判不安全）
    ++mWaitTicks;
    auto dim = level->getDimension((::DimensionType)mDim).lock();
    if (!dim) {
        mAborted++;
        releaseArea(mArea, mDim);
        mArea.clear();
        mPhase    = 0;
        mCooldown = 20;
        return;
    }
    if (!isChunkReady(*dim, mX, mZ)) {
        if (mWaitTicks > kWaitLimitTicks) {
            mAborted++;
            log().warn("[RTP][预热] 点 ({}, {}) 等 {} tick 仍未就绪（最后状态={}）→ 放弃本次",
                       mX, mZ, mWaitTicks, chunkStateName(chunkStateAt(*dim, mX, mZ)));
            releaseArea(mArea, mDim);
            mArea.clear();
            mPhase    = 0;
            mCooldown = 20;
        }
        return;
    }

    std::unordered_set<std::string> dangerSet;
    for (auto& b : cfg.dangerBlocks()) dangerSet.insert(b);
    SafePos    pos{};
    bool const ok = scanChunkMemory(*dim, mDim, mX >> 4, mZ >> 4, getDimYRange(mDim),
                                    getScanStartY(mDim), dangerSet, pos);
    finishWarm(ok, ok ? (int)std::floor(pos.y) : 0);
}


} // namespace mtps
