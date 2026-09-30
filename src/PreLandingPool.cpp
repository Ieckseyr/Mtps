// PreLandingPool.cpp - 预落点池实现（设计见 docs/MTPS随机传送预落点优化方案.md）
//
// 线程模型：
//   主线程   pick() / markVerified() / markInvalid()  —— pick 拿共享锁（只读、有界），
//                                                         mark* 拿排他锁（罕见）
//   后台线程 topUp()                                  —— 重活在锁外算，只在"取快照"和
//                                                        "写回"两处短持排他锁
// 这样 topUp 期间（可能几百毫秒）主线程的 pick 不会被长时间阻塞。
#include "PreLandingPool.h"

#include "ArchiveScanner.h"
#include "BiomeSampler.h"
#include "Config.h"

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <unordered_set>

namespace mtps {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr char   kMagic[8] = {'M', 'T', 'P', 'P', 'O', 'O', 'L', '1'};
constexpr uint32_t kFormatVersion = 1;

ll::io::Logger& poolLogger() {
    return ll::mod::NativeMod::current()->getLogger();
}

inline uint64_t mix64(uint64_t& x) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline uint32_t nowSec() { return (uint32_t)std::time(nullptr); }

// 桶坐标（负数向下取整；不能直接用 / 的截断语义）
inline int64_t floorDiv(int64_t v, int64_t d) {
    int64_t q = v / d;
    if ((v % d != 0) && ((v < 0) != (d < 0))) q--;
    return q;
}

} // namespace

PreLandingPool& PreLandingPool::getInstance() {
    static PreLandingPool instance;
    return instance;
}

void PreLandingPool::setOptions(PoolOptions const& o) {
    std::unique_lock lk(mMutex);
    mOpts = o;
}

PoolOptions PreLandingPool::options() const {
    std::shared_lock lk(mMutex);
    return mOpts;
}

int64_t PreLandingPool::bucketKey(int bx, int bz) {
    return ((int64_t)bx << 32) ^ (int64_t)(uint32_t)bz;
}

void PreLandingPool::rebuildIndexLocked(int dimid) {
    DimData& d = mDims[dimid];
    d.buckets.clear();
    d.nonEmpty.clear();
    int const bs = std::max(1, mOpts.bucketSize);
    d.buckets.reserve(d.points.size() / 2 + 8);
    d.nonEmpty.reserve(d.points.size() / 2 + 8);
    for (uint32_t i = 0; i < (uint32_t)d.points.size(); i++) {
        PreLanding const& p = d.points[i];
        int64_t const k = bucketKey((int)floorDiv(p.x, bs), (int)floorDiv(p.z, bs));
        auto [it, inserted] = d.buckets.try_emplace(k);
        if (inserted) d.nonEmpty.push_back(k);
        it->second.push_back(i);
    }
}

void PreLandingPool::addPointLocked(int dimid, PreLanding const& p) {
    DimData& d = mDims[dimid];
    int const bs = std::max(1, mOpts.bucketSize);
    int64_t const k = bucketKey((int)floorDiv(p.x, bs), (int)floorDiv(p.z, bs));
    auto [it, inserted] = d.buckets.try_emplace(k);
    if (inserted) d.nonEmpty.push_back(k);
    it->second.push_back((uint32_t)d.points.size());
    d.points.push_back(p);
}

// 圆盘内可能有权重点的桶。桶数少就按桶网格枚举（精确），桶数多就遍历非空桶（有界）。
std::vector<int64_t> PreLandingPool::candidateBucketsLocked(int dimid, double ox, double oz,
                                                            int radius) const {
    DimData const& d = mDims[dimid];
    std::vector<int64_t> keys;
    int const bs = std::max(1, mOpts.bucketSize);
    if (d.nonEmpty.empty() || radius <= 0) return keys;

    int64_t const minBX = floorDiv((int64_t)std::floor(ox - radius), bs);
    int64_t const maxBX = floorDiv((int64_t)std::ceil(ox + radius), bs);
    int64_t const minBZ = floorDiv((int64_t)std::floor(oz - radius), bs);
    int64_t const maxBZ = floorDiv((int64_t)std::ceil(oz + radius), bs);
    int64_t const spanX = maxBX - minBX + 1;
    int64_t const spanZ = maxBZ - minBZ + 1;

    if (spanX > 0 && spanZ > 0 && spanX * spanZ <= 65536) {
        // 桶网格不大：直接枚举圆所覆盖的桶（O(桶数)，随手跳过空桶）
        for (int64_t bx = minBX; bx <= maxBX; bx++) {
            for (int64_t bz = minBZ; bz <= maxBZ; bz++) {
                int64_t const k = bucketKey((int)bx, (int)bz);
                if (d.buckets.find(k) != d.buckets.end()) keys.push_back(k);
            }
        }
        return keys;
    }

    // 半径很大（桶网格会爆）：遍历非空桶，按桶中心粗筛在圆内（O(非空桶数) ≤ O(点数)）
    int64_t const r2 = (int64_t)radius * radius;
    for (int64_t k : d.nonEmpty) {
        auto it = d.buckets.find(k);
        if (it == d.buckets.end() || it->second.empty()) continue;
        // 桶中心 = 任一成员点所在桶的中心；用桶 key 还原
        int32_t const bx = (int32_t)(k >> 32);
        int32_t const bz = (int32_t)(uint32_t)k;
        double const cx = ((double)bx + 0.5) * bs;
        double const cz = ((double)bz + 0.5) * bs;
        double const ddx = cx - ox, ddz = cz - oz;
        // 桶中心到原点距离 ≤ r + 半桶对角线 ⇒ 桶内可能有点落在圆内
        double const pad = bs * 0.7072;
        if (std::sqrt(ddx * ddx + ddz * ddz) <= radius + pad) keys.push_back(k);
        (void)r2;
    }
    return keys;
}

void PreLandingPool::setSeedInfo(int64_t seed, int cubiomesMc) {
    std::unique_lock lk(mMutex);
    mSeed = seed;
    mMc   = cubiomesMc;
}

bool PreLandingPool::save() const {
    std::vector<PreLanding> pts[3];
    PoolOptions             opt;
    int64_t                 seed;
    int                     mc;
    {
        std::shared_lock lk(mMutex);
        opt  = mOpts;
        seed = mSeed;
        mc   = mMc;
        for (int i = 0; i < 3; i++) pts[i] = mDims[i].points;
    }
    if (!saveMeta()) return false;

    std::vector<uint8_t> buf;
    buf.resize(20);
    std::memcpy(buf.data(), kMagic, 8);
    uint32_t v = kFormatVersion, slot = (uint32_t)sizeof(PreLanding), nd = 3;
    std::memcpy(buf.data() + 8, &v, 4);
    std::memcpy(buf.data() + 12, &slot, 4);
    std::memcpy(buf.data() + 16, &nd, 4);
    for (int i = 0; i < 3; i++) {
        uint64_t n = pts[i].size();
        size_t const at = buf.size();
        buf.resize(at + 8 + (size_t)n * sizeof(PreLanding));
        std::memcpy(buf.data() + at, &n, 8);
        if (n) std::memcpy(buf.data() + at + 8, pts[i].data(), (size_t)n * sizeof(PreLanding));
    }
    uLong const crc = crc32(0, buf.data(), (uInt)buf.size());
    uint32_t const crc32v = (uint32_t)crc;
    buf.resize(buf.size() + 4);
    std::memcpy(buf.data() + buf.size() - 4, &crc32v, 4);

    auto const path = Config::dataDir() / opt.file;
    auto const tmp  = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f.write((char const*)buf.data(), (std::streamsize)buf.size());
        if (!f.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    return !ec;
}

bool PreLandingPool::saveMeta() const {
    nlohmann::json j;
    PoolOptions    opt;
    {
        std::shared_lock lk(mMutex);
        opt                = mOpts;
        j["version"]       = kFormatVersion;
        j["seed"]          = mSeed;
        j["mc"]            = mMc;
        j["savedAt"]       = (int64_t)std::time(nullptr);
        j["targets"]       = {{"loaded", opt.loadedTarget}, {"unloaded", opt.unloadedTarget}};
        j["rules"]         = kLandingRulesVersion;   // 取面规则版本（变了就重收已加载类）
        auto dims          = nlohmann::json::array();
        for (int i = 0; i < 3; i++) {
            DimData const& d = mDims[i];
            if (!d.present) continue;
            dims.push_back({{"dim", i},
                            {"ox", d.originX},
                            {"oz", d.originZ},
                            {"radius", d.radius},
                            {"builtAt", d.builtAt},
                            {"present", true},
                            {"pending", d.pending},
                            {"pendingRadius", d.pendingRadius},
                            {"pendingOx", d.pendingOriginX},
                            {"pendingOz", d.pendingOriginZ}});
        }
        j["dims"] = dims;
    }
    auto const path = Config::dataDir() / (opt.file + ".meta");
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << j.dump(2);
    return f.good();
}

bool PreLandingPool::loadMeta() {
    PoolOptions opt;
    {
        std::shared_lock lk(mMutex);
        opt = mOpts;
    }
    auto const path = Config::dataDir() / (opt.file + ".meta");
    std::ifstream f(path);
    if (!f.is_open()) return false;
    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        return false;
    }
    if (!j.is_object() || !j.contains("dims")) return false;

    // 种子/mc 不符 → 整个池作废（群系推算的结论变了；已加载类也随之重来，
    // 反正它们是从落点表重新采样出来的，没有额外成本）
    int64_t const seed = j.value("seed", (int64_t)0);
    int const     mc   = j.value("mc", -1);
    if (seed != mSeed || mc != mMc) return false;
    // 取面规则变了（例如"树冠不算地表"）→ 已加载类的 approxY 过时, 标一下让 topUp 重收。
    // 不能整体作废: 未加载类那批点是 cubiomes 按种子推算的, 与取面规则无关, 重算一遍纯浪费。
    bool const rulesStale = (j.value("rules", std::string{}) != kLandingRulesVersion);

    std::unique_lock lk(mMutex);
    mTier0Stale = rulesStale;
    for (int i = 0; i < 3; i++) {
        mDims[i].present  = false;
        mDims[i].pending  = false;
        mDims[i].radius   = 0;
    }
    for (auto const& dd : j["dims"]) {
        int const dim = dd.value("dim", -1);
        if (dim < 0 || dim > 2) continue;
        DimData& d       = mDims[dim];
        d.present        = dd.value("present", false);
        d.originX        = dd.value("ox", 0.0);
        d.originZ        = dd.value("oz", 0.0);
        d.radius         = dd.value("radius", 0);
        d.builtAt        = dd.value("builtAt", (int64_t)0);
        d.pending        = dd.value("pending", false);
        d.pendingRadius  = dd.value("pendingRadius", 0);
        d.pendingOriginX = dd.value("pendingOx", 0.0);
        d.pendingOriginZ = dd.value("pendingOz", 0.0);
    }
    return true;
}

bool PreLandingPool::load() {
    // 先确认指纹可用（种子/mc 由调用方在这之前 Biomesampler::configure 时就绪）
    {
        std::unique_lock lk(mMutex);
        if (!mOpts.enabled) {
            mLoaded = true;   // "已加载"但空池，pick 会直接失败 → 走降级
            return true;
        }
    }
    if (!loadMeta()) {
        // meta 不存在或指纹不符：把指纹刷成当前值，等 topUp 全量建池
        std::unique_lock lk(mMutex);
        mDims[0] = DimData{};
        mDims[1] = DimData{};
        mDims[2] = DimData{};
        mLoaded  = true;
        return false;
    }

    PoolOptions opt;
    {
        std::shared_lock lk(mMutex);
        opt = mOpts;
    }
    auto const path = Config::dataDir() / opt.file;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::unique_lock lk(mMutex);
        mLoaded = true;
        return false;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < 24) {
        std::unique_lock lk(mMutex);
        mLoaded = true;
        return false;
    }
    if (std::memcmp(buf.data(), kMagic, 8) != 0) {
        std::unique_lock lk(mMutex);
        mLoaded = true;
        return false;
    }
    uint32_t v = 0, slot = 0, nd = 0;
    std::memcpy(&v, buf.data() + 8, 4);
    std::memcpy(&slot, buf.data() + 12, 4);
    std::memcpy(&nd, buf.data() + 16, 4);
    if (v != kFormatVersion || slot != sizeof(PreLanding) || nd != 3) {
        std::unique_lock lk(mMutex);
        mLoaded = true;
        return false;
    }
    // CRC 校验（池是"花时间算出来的资产"，坏了要能识别并重建，不能带着残文件跑）
    uint32_t crcStored = 0;
    std::memcpy(&crcStored, buf.data() + buf.size() - 4, 4);
    uLong const crcCalc = crc32(0, buf.data(), (uInt)(buf.size() - 4));
    if ((uint32_t)crcCalc != crcStored) {
        poolLogger().warn("[RTP][预落点池] 缓存 CRC 校验失败（文件损坏）→ 重新建池");
        std::unique_lock lk(mMutex);
        mLoaded = true;
        return false;
    }

    size_t off = 20;
    std::unique_lock lk(mMutex);
    for (int i = 0; i < 3; i++) {
        uint64_t n = 0;
        if (off + 8 > buf.size() - 4) break;
        std::memcpy(&n, buf.data() + off, 8);
        off += 8;
        if (n > 4'000'000ull || off + (size_t)n * sizeof(PreLanding) > buf.size() - 4) break;
        mDims[i].points.resize((size_t)n);
        if (n) std::memcpy(mDims[i].points.data(), buf.data() + off, (size_t)n * sizeof(PreLanding));
        off += (size_t)n * sizeof(PreLanding);
    }
    for (int i = 0; i < 3; i++) rebuildIndexLocked(i);
    mLoaded = true;
    return true;
}

void PreLandingPool::unload() {
    std::unique_lock lk(mMutex);
    for (int i = 0; i < 3; i++) {
        mDims[i] = DimData{};
    }
    mLoaded = false;
}

void PreLandingPool::setRegions(std::vector<Region> const& regions) {
    bool   metaDirty = false;
    struct Changed { int dim; int from; int to; double ox, oz; } changed[3];
    int    changedN = 0;
    double replaceRatio = 0.5;
    {
        std::unique_lock lk(mMutex);
        replaceRatio = mOpts.replaceRatio;
        for (auto const& r : regions) {
            if (r.dimid < 0 || r.dimid > 2 || !r.valid) continue;
            DimData& d = mDims[r.dimid];
            if (d.present && d.originX == r.originX && d.originZ == r.originZ && d.radius == r.radius
                && !d.pending) {
                continue;   // 没变
            }
            if (d.radius > 0 && d.present && mLoaded && mOpts.rebuildOnRadiusChange) {
                // 建过池了且区域变了 → 打"待重建"标记。真正的重算在后台 topUp 里做，
                // 这里只动内存 + 几十字节的 meta，表单保存秒回、不打断正在进行的传送。
                if (d.pending && d.pendingRadius == r.radius && d.pendingOriginX == r.originX
                    && d.pendingOriginZ == r.originZ) {
                    continue;
                }
                d.pending        = true;
                d.pendingOriginX = r.originX;
                d.pendingOriginZ = r.originZ;
                d.pendingRadius  = r.radius;
                metaDirty        = true;
                if (changedN < 3) changed[changedN++] = {r.dimid, d.radius, r.radius, r.originX, r.originZ};
            } else {
                // 还没建过池：直接记为目标区域
                d.originX = r.originX;
                d.originZ = r.originZ;
                d.radius  = r.radius;
            }
        }
    }
    for (int i = 0; i < changedN; i++) {
        poolLogger().info("[RTP][预落点池] 维度 {} 区域变更: 半径 {} → {}（起点 {:.0f},{:.0f}）"
                          "→ 记下待重建, 后台静默重算未加载类并替换 {:.0f}% 旧点, 不打断当前传送",
                          changed[i].dim, changed[i].from, changed[i].to, changed[i].ox, changed[i].oz,
                          replaceRatio * 100.0);
    }
    if (metaDirty) saveMeta();   // 只写 meta，秒回；重启也不会丢这个标记
}

PreLandingPool::Region PreLandingPool::regionOf(int dimid) const {
    Region r;
    if (dimid < 0 || dimid > 2) return r;
    std::shared_lock lk(mMutex);
    DimData const&   d = mDims[dimid];
    if (d.pending) return {dimid, d.pendingOriginX, d.pendingOriginZ, d.pendingRadius, true};
    // "有半径就算有效"：池可能还没建（present=false），但目标区域已经从配置同步过来了
    if (d.radius > 0) return {dimid, d.originX, d.originZ, d.radius, true};
    return r;
}

bool PreLandingPool::rebuildPending(int dimid) const {
    if (dimid < 0 || dimid > 2) return false;
    std::shared_lock lk(mMutex);
    return mDims[dimid].pending;
}

bool PreLandingPool::usable() const {
    std::shared_lock lk(mMutex);
    if (!mLoaded || !mOpts.enabled) return false;
    for (int i = 0; i < 3; i++) {
        if (!mDims[i].points.empty()) return true;
    }
    return false;
}

size_t PreLandingPool::count(int dimid, int tier, bool onlyUsable) const {
    if (dimid < 0 || dimid > 2) return 0;
    std::shared_lock lk(mMutex);
    size_t            n = 0;
    for (PreLanding const& p : mDims[dimid].points) {
        if (tier >= 0 && p.tier != (uint8_t)tier) continue;
        if (onlyUsable && p.state == kPlStateInvalid) continue;
        n++;
    }
    return n;
}

bool PreLandingPool::pick(int dimid, double ox, double oz, int radius, uint64_t seed,
                          int avoidX, int avoidZ, int avoidR, Pick& out, int wantTier) {
    if (dimid < 0 || dimid > 2 || radius <= 0) return false;

    // 常驻点优先：区块已经在内存里（预热时生成好并留着），落上去是瞬时的。同样要满足圆内、
    // 离上次落点够远、类别符合，不满足就照常走下面的抽取——所以不是永远落在固定几个点。
    // 用完即被消费（升级为已加载类），常驻位由预热器补下一个。
    {
        std::shared_lock lk(mMutex);
        if (mLoaded && mOpts.enabled && !mResident.empty()) {
            int64_t const r2 = (int64_t)radius * radius;
            for (PreLanding const& p : mResident) {
                if (wantTier >= 0 && p.tier != (uint8_t)wantTier) continue;
                if (p.state == kPlStateInvalid) continue;
                int64_t const dx = (int64_t)p.x - (int64_t)ox;
                int64_t const dz = (int64_t)p.z - (int64_t)oz;
                if (dx * dx + dz * dz > r2) continue;
                if (avoidR > 0) {
                    int64_t const ax = (int64_t)p.x - avoidX;
                    int64_t const az = (int64_t)p.z - avoidZ;
                    if (ax * ax + az * az < (int64_t)avoidR * avoidR) continue;
                }
                out.x = p.x; out.z = p.z; out.targetX = p.x; out.targetZ = p.z;
                out.tier = p.tier; out.biome = p.biome; out.hintY = p.approxY;
                return true;
            }
        }
    }

    PreLanding chosen{};
    bool       has = false;
    {
        std::shared_lock lk(mMutex);
        if (!mLoaded || !mOpts.enabled) return false;
        DimData const& d = mDims[dimid];
        if (d.points.empty()) return false;

        uint64_t      rng = seed;
        int           n   = 0;
        int64_t const r2  = (int64_t)radius * radius;
        // 间隔约束（同一玩家上次落点）：先把"够远"的点挑一遍，一个都没有再退回不过滤。
        int64_t const avoid2 = (avoidR > 0) ? (int64_t)avoidR * avoidR : 0;
        bool          farPass = avoid2 > 0;   // 第一遍只收"离上次落点够远"的

        // 蓄水池采样：等概率取一个可用点
        auto feed = [&](int64_t k, int64_t limit2, int cx, int cz) {
            auto it = d.buckets.find(k);
            if (it == d.buckets.end()) return;
            for (uint32_t idx : it->second) {
                if (idx >= d.points.size()) continue;
                PreLanding const& p = d.points[idx];
                if (p.state == kPlStateInvalid) continue;
                if (wantTier >= 0 && p.tier != (uint8_t)wantTier) continue;
                int64_t const dx  = (int64_t)p.x - cx;
                int64_t const dz  = (int64_t)p.z - cz;
                int64_t const d2  = dx * dx + dz * dz;
                if (d2 > limit2) continue;
                if (farPass && avoid2 > 0) {
                    int64_t const ax = (int64_t)p.x - avoidX;
                    int64_t const az = (int64_t)p.z - avoidZ;
                    if (ax * ax + az * az < avoid2) continue;   // 离上次落点太近
                }
                n++;
                if (mix64(rng) % (uint64_t)n == 0) {
                    chosen = p;
                    has    = true;
                }
            }
        };

        // 桶就是"大致区域"：随机挑桶 + 桶内取点，命中率与半径无关，大半径下也真能命中
        auto collectAndFeed = [&] {
            auto const keys = candidateBucketsLocked(dimid, ox, oz, radius);
            for (int64_t k : keys) feed(k, r2, (int)ox, (int)oz);
        };

        collectAndFeed();
        if (!has && farPass) {
            // 全都在上次落点附近（已加载类聚集在已探索区时会出现）→ 放开间隔约束重来一遍
            farPass = false;
            n       = 0;
            collectAndFeed();
        }
        if (!has) return false;
    }

    out.x       = chosen.x;
    out.z       = chosen.z;
    out.targetX = chosen.x;
    out.targetZ = chosen.z;
    out.tier    = chosen.tier;
    out.biome   = chosen.biome;
    out.hintY   = chosen.approxY;

    // 池点**就是**目标点：不做抖动。
    //
    // 池点就是目标点，不做抖动。以前会在 500 格内随机偏一下防蹲点，但偏几百格经常跨过区块边界，
    // 真正被生成、被验证的成了另一个 chunk，回写状态却按池点所在 chunk 判断，于是成功的传送被误判成
    // "该 chunk 无安全列"（日志里真出现过）。防蹲点改由"用过即升级"承担。
    return true;
}

void PreLandingPool::markVerified(int dimid, int x, int z, int realY) {
    if (dimid < 0 || dimid > 2) return;
    {
        std::unique_lock lk(mMutex);
        for (PreLanding& p : mDims[dimid].points) {
            if (p.x != x || p.z != z) continue;
            if (p.tier == kPlTierUnloaded) {
                // 自愈升级：这个点已经被生成过并扫描通过 → 它有存档数据了，归入已加载类。
                // 之后传送只需载入区块，不再需要生成。
                p.tier     = kPlTierLoaded;
                p.state    = kPlStateVerified;
                p.approxY  = realY;
                p.lastUsed = nowSec();
            }
            break;
        }
    }
}

void PreLandingPool::markInvalid(int dimid, int x, int z) {
    if (dimid < 0 || dimid > 2) return;
    {
        std::unique_lock lk(mMutex);
        for (PreLanding& p : mDims[dimid].points) {
            if (p.x != x || p.z != z) continue;
            if (p.state != kPlStateInvalid) {
                p.state    = kPlStateInvalid;
                p.lastUsed = nowSec();
            }
            break;
        }
    }
}

void PreLandingPool::setResident(std::vector<PreLanding> const& pts) {
    std::unique_lock lk(mMutex);
    mResident = pts;
}

bool PreLandingPool::takeWarmCandidate(int dimid, double ox, double oz, int radius, uint64_t seed,
                                       std::vector<std::pair<int, int>> const& recent, Pick& out) {
    std::vector<PreLanding> cands;
    {
        std::shared_lock lk(mMutex);
        if (!mLoaded || !mOpts.enabled || dimid < 0 || dimid > 2) return false;
        auto const& d = mDims[dimid];
        if (d.points.empty()) return false;

        int64_t const    r2 = (int64_t)radius * radius;
        auto const       keys = candidateBucketsLocked(dimid, ox, oz, radius);
        uint64_t         rng  = seed;
        int              n    = 0;
        PreLanding       chosen{};
        bool             has  = false;
        for (int64_t k : keys) {
            auto it = d.buckets.find(k);
            if (it == d.buckets.end()) continue;
            for (uint32_t idx : it->second) {
                if (idx >= d.points.size()) continue;
                PreLanding const& p = d.points[idx];
                if (p.state == kPlStateInvalid) continue;
                if (p.tier != kPlTierUnloaded) continue;   // 只预热"待生成"的点
                int64_t const dx = (int64_t)p.x - (int64_t)ox;
                int64_t const dz = (int64_t)p.z - (int64_t)oz;
                if (dx * dx + dz * dz > r2) continue;
                bool dup = false;
                for (auto const& r : recent) {
                    if (r.first == p.x && r.second == p.z) { dup = true; break; }
                }
                if (dup) continue;   // 刚预热过（还常驻着）的不重复预热
                n++;
                if (mix64(rng) % (uint64_t)n == 0) { chosen = p; has = true; }
            }
        }
        if (!has) return false;
        out.x = chosen.x; out.z = chosen.z; out.targetX = chosen.x; out.targetZ = chosen.z;
        out.tier = chosen.tier; out.biome = chosen.biome; out.hintY = chosen.approxY;
        return true;
    }
}

std::vector<PreLanding> PreLandingPool::generateUnloaded(int dimid, Region const& reg, size_t want,
                                                         std::vector<PreLanding> const& avoid) {
    std::vector<PreLanding> out;
    if (!reg.valid || reg.radius <= 0 || want == 0) return out;
    auto& sampler = BiomeSampler::getInstance();
    if (!sampler.ready()) return out;

    PoolOptions opt;
    {
        std::shared_lock lk(mMutex);
        opt = mOpts;
    }
    int const     bs   = std::max(1, opt.bucketSize);
    int64_t const sep2 = (int64_t)std::max(1, opt.minSeparation) * std::max(1, opt.minSeparation);

    // 局部桶索引：候选点与"已有保留点"都放进来，只查 3×3 邻桶 → O(1) 级距离判定
    std::unordered_map<int64_t, std::vector<std::pair<int, int>>> grid;
    auto insertGrid = [&](int x, int z) {
        grid[bucketKey((int)floorDiv(x, bs), (int)floorDiv(z, bs))].push_back({x, z});
    };
    for (PreLanding const& p : avoid) {
        if (p.state != kPlStateInvalid) insertGrid(p.x, p.z);
    }
    auto tooCloseLocal = [&](int x, int z) -> bool {
        int64_t const bx = floorDiv(x, bs), bz = floorDiv(z, bs);
        for (int64_t dx = -1; dx <= 1; dx++) {
            for (int64_t dz = -1; dz <= 1; dz++) {
                auto it = grid.find(bucketKey((int)(bx + dx), (int)(bz + dz)));
                if (it == grid.end()) continue;
                for (auto const& q : it->second) {
                    int64_t const ddx = (int64_t)q.first - x, ddz = (int64_t)q.second - z;
                    if (ddx * ddx + ddz * ddz < sep2) return true;
                }
            }
        }
        return false;
    };

    size_t const maxTries = want * (size_t)std::max(1, opt.maxTriesPerPoint);
    std::mt19937_64 rng((uint64_t)std::time(nullptr) * 6364136223846793005ull
                        ^ ((uint64_t)dimid << 32) ^ (uint64_t)reg.radius);
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    out.reserve(want);
    for (size_t t = 0; t < maxTries && out.size() < want; t++) {
        double const ang  = u01(rng) * 2.0 * kPi;
        double const dist = std::sqrt(u01(rng)) * (double)reg.radius;   // 圆盘上按面积均匀
        int const    x    = (int)std::lround(reg.originX + std::cos(ang) * dist);
        int const    z    = (int)std::lround(reg.originZ + std::sin(ang) * dist);

        if (tooCloseLocal(x, z)) continue;   // 便宜的检查先做
        auto const s = sampler.sample(x, z);
        if (!s.ok || s.excluded) continue;
        if (!s.hasApproxY) continue;
        if (s.approxY < (float)opt.approxHeightMin || s.approxY > 320.0f) continue;

        PreLanding p{};
        p.x        = x;
        p.z        = z;
        p.approxY  = (int32_t)std::lround(s.approxY);
        p.biome    = (uint16_t)(s.biome < 0 ? 0 : s.biome);
        p.tier     = kPlTierUnloaded;
        p.state    = kPlStateCandidate;
        p.lastUsed = 0;
        insertGrid(x, z);
        out.push_back(p);
    }
    return out;
}

std::vector<PreLanding> PreLandingPool::harvestLoaded(int dimid, Region const& reg, size_t want,
                                                      std::vector<PreLanding> const& avoid,
                                                      HarvestStats& st) {
    std::vector<PreLanding> out;
    st = HarvestStats{};
    auto table = ArchiveScanner::getInstance().snapshotLandings(dimid);
    if (table.empty() || want == 0) return out;
    auto& sampler = BiomeSampler::getInstance();
    if (!sampler.ready()) return out;

    // 考察预算：每点一次群系采样（≈90µs），所以不能把整张大表都过一遍。
    // 小表（本服 1312 条）stride=1, 全都看；大表按 want*4 等距看一遍。
    size_t const examineBudget = std::max<size_t>(want * 4, 512);
    size_t const stride = std::max<size_t>(1, table.size() / std::max<size_t>(1, examineBudget));
    int64_t const r2   = (int64_t)reg.radius * reg.radius;

    // 已在池里的点（必须去重）: 否则每次开服都会把同一批落点再收一遍 —— 池里堆满
    // 重复坐标, 抽取时它们被命中的概率成倍放大, 且库存数字虚高看不出问题。
    std::unordered_set<int64_t> existing;
    existing.reserve(avoid.size() * 2 + 16);
    for (PreLanding const& p : avoid) {
        existing.insert(((int64_t)p.x << 32) ^ (int64_t)(uint32_t)p.z);
    }
    auto posKey = [](int x, int z) { return ((int64_t)x << 32) ^ (int64_t)(uint32_t)z; };

    // ── 第一遍：挑出全部合格候选（LAND_SAFE + 在区域内 + 群系合格 + 不在池里）──
    struct Cand { int x, z, y, biome; };
    std::vector<Cand> cands;
    cands.reserve(std::min<size_t>(table.size(), examineBudget));
    for (size_t i = 0; i < table.size(); i += stride) {
        st.examined++;
        auto const& ld = table[i];
        if (ld.state != BedrockLevelReader::LAND_SAFE) continue;
        int const x = ld.cx * 16 + (ld.lx >= 0 && ld.lx < 16 ? ld.lx : 8);
        int const z = ld.cz * 16 + (ld.lz >= 0 && ld.lz < 16 ? ld.lz : 8);
        if (existing.count(posKey(x, z))) {
            st.already++;
            continue;
        }
        if (reg.valid && reg.radius > 0) {
            int64_t const dx = (int64_t)x - (int64_t)reg.originX;
            int64_t const dz = (int64_t)z - (int64_t)reg.originZ;
            if (dx * dx + dz * dz > r2) continue;
        }
        auto const s = sampler.sample(x, z);
        if (!s.ok || s.excluded) continue;   // 已加载类也走同一套群系排除（口径统一）
        cands.push_back({x, z, ld.y, s.biome < 0 ? 0 : s.biome});
    }
    st.qualified = cands.size();
    if (cands.empty()) return out;

    // ── 第二遍：在合格候选里等距取样 want 个 ──
    // （不能直接取前 want 个：表按 (cx,cz) 排序, 前 want 个会全挤在世界的西边。）
    size_t const pick2 = std::max<size_t>(1, cands.size() / std::max<size_t>(1, want));
    out.reserve(std::min(want, cands.size()));
    for (size_t i = 0; i < cands.size() && out.size() < want; i += pick2) {
        auto const& c  = cands[i];
        PreLanding  p{};
        p.x        = c.x;
        p.z        = c.z;
        p.approxY  = c.y;                    // 已加载类存真实站立 Y
        p.biome    = (uint16_t)c.biome;
        p.tier     = kPlTierLoaded;
        p.state    = kPlStateVerified;       // 来自落点表 = 已知安全，直接可用
        p.lastUsed = 0;
        out.push_back(p);
    }
    (void)avoid;   // 已加载类不查最小间距（见头文件说明）
    return out;
}

PreLandingPool::TopUpResult PreLandingPool::topUp(int dimid) {
    TopUpResult res;
    if (dimid < 0 || dimid > 2) return res;

    PoolOptions opt;
    Region      reg;
    bool        pending = false;
    {
        std::shared_lock lk(mMutex);
        if (!mLoaded || !mOpts.enabled) return res;
        opt = mOpts;
        DimData const& d = mDims[dimid];
        if (d.pending) {
            pending = true;
            reg     = {dimid, d.pendingOriginX, d.pendingOriginZ, d.pendingRadius, true};
        } else if (d.radius > 0) {
            reg = {dimid, d.originX, d.originZ, d.radius, true};
        }
        if (!reg.valid) return res;   // 这个维度没有启用预设 → 不需要池
    }
    res.ok = true;

    // ① 清理与淘汰（短持排他锁，只动内存）
    std::vector<PreLanding> keep;
    bool                    tier0Stale = false;
    {
        std::unique_lock lk(mMutex);
        DimData&           d = mDims[dimid];
        tier0Stale           = mTier0Stale;
        mTier0Stale          = false;   // 只重收一次（本维度的收完就清；多维度时下一维度仍会重收）
        size_t             invalidDropped = 0;
        size_t             loadedCnt = 0, unloadedCnt = 0;
        for (PreLanding const& p : d.points) {
            if (p.state == kPlStateInvalid) {
                invalidDropped++;
                continue;   // 失效点直接丢，缺口由本轮补齐填上
            }
            // 取面规则变了 → 已加载类的 approxY 过时, 全丢重收（见 loadMeta）
            if (tier0Stale && p.tier == kPlTierLoaded) {
                res.dropped++;
                continue;
            }
            keep.push_back(p);
        }
        res.dropped += invalidDropped;

        // 目标区域外的点：无条件清掉（它们在 pick 时会被距离过滤掉，纯占名额 ——
        // 半径缩小后必然出现这种点）。两类都清：
        //   · 未加载类的清理计入下面的"替换一半"额度（避免重复淘汰）
        //   · 已加载类的清理单独计数, 缺口由本轮的 harvestLoaded 补回来
        size_t removedForRegion = 0;
        if (reg.valid && reg.radius > 0) {
            int64_t const r2 = (int64_t)reg.radius * reg.radius;
            std::vector<PreLanding> tmp;
            tmp.reserve(keep.size());
            for (PreLanding const& p : keep) {
                int64_t const dx = (int64_t)p.x - (int64_t)reg.originX;
                int64_t const dz = (int64_t)p.z - (int64_t)reg.originZ;
                if (dx * dx + dz * dz > r2) {
                    if (p.tier == kPlTierUnloaded) removedForRegion++;
                    else                           res.outOfRegion++;
                    continue;
                }
                tmp.push_back(p);
            }
            keep.swap(tmp);
        }

        // 半径重建：让出位置给新区域（replaceRatio）。已经因为"在区域外"清掉的算在额度里，
        // 所以缩小半径时不会把在区域内、还能用的点也一起淘汰掉。
        if (pending) {
            size_t const total   = (size_t)opt.unloadedTarget;
            size_t const replace = (size_t)std::llround((double)total * opt.replaceRatio);
            size_t       removed = removedForRegion;
            if (removed < replace) {
                std::vector<PreLanding> tmp;
                tmp.reserve(keep.size());
                for (PreLanding const& p : keep) {
                    if (p.tier == kPlTierUnloaded && removed < replace) {
                        removed++;   // 从最老的（前面的）开始让位
                        continue;
                    }
                    tmp.push_back(p);
                }
                keep.swap(tmp);
            }
            res.unloadedReplaced = removed;
            res.rebuilt          = true;
        }

        // 超目标淘汰（顺序 = 加入顺序，前面的最老；已加载类淘汰是无损的 ——
        // 它们本来就是从落点表采样出来的，随时能再采）
        {
            std::vector<PreLanding> tmp;
            tmp.reserve(keep.size());
            for (PreLanding const& p : keep) {
                if (p.tier == kPlTierLoaded) {
                    loadedCnt++;
                    if (loadedCnt > (size_t)opt.loadedTarget) {
                        res.evicted++;
                        continue;
                    }
                } else {
                    unloadedCnt++;
                    if (unloadedCnt > (size_t)opt.unloadedTarget) {
                        res.evicted++;
                        continue;
                    }
                }
                tmp.push_back(p);
            }
            keep.swap(tmp);
        }
        // 先把清理结果落地（生成期间新来的 mark* 作用在 keep 上是安全的：
        // keep 里是同一批点，且我们下面才 swap 进去）
        d.points.swap(keep);
        rebuildIndexLocked(dimid);
        if (pending) {
            d.pending        = false;
            d.pendingRadius  = 0;
            d.pendingOriginX = 0;
            d.pendingOriginZ = 0;
        }
    }

    // ② 未加载类：cubiomes 推算（重活，锁外）
    {
        std::vector<PreLanding> snapshot;
        {
            std::shared_lock lk(mMutex);
            snapshot = mDims[dimid].points;
        }
        size_t unloadedCnt = 0;
        for (PreLanding const& p : snapshot) {
            if (p.tier == kPlTierUnloaded) unloadedCnt++;
        }
        // 半径容量上限：圆盘内按最小间距能放下的点数 ≈ πR²/s²。
        // 目标比容量大时自动下调（否则生成循环会一直尝试到尝试上限，白跑）。
        size_t effectiveTarget = (size_t)std::max(0, opt.unloadedTarget);
        if (reg.valid && reg.radius > 0) {
            double const cap = kPi * (double)reg.radius * (double)reg.radius
                             / ((double)std::max(1, opt.minSeparation) * (double)std::max(1, opt.minSeparation));
            if (cap < (double)effectiveTarget) effectiveTarget = (size_t)cap;
        }
        res.effectiveUnloadedTarget = effectiveTarget;
        if (unloadedCnt < effectiveTarget) {
            auto added = generateUnloaded(dimid, reg, effectiveTarget - unloadedCnt, snapshot);
            res.unloadedAdded = added.size();
            if (!added.empty()) {
                std::unique_lock lk(mMutex);
                for (PreLanding const& p : added) addPointLocked(dimid, p);
            }
        }
    }

    // ③ 已加载类：从落点表等距采样（重活，锁外）
    {
        std::vector<PreLanding> snapshot;
        {
            std::shared_lock lk(mMutex);
            snapshot = mDims[dimid].points;
        }
        size_t loadedCnt = 0;
        for (PreLanding const& p : snapshot) {
            if (p.tier == kPlTierLoaded) loadedCnt++;
        }
        if (loadedCnt < (size_t)opt.loadedTarget) {
            HarvestStats st{};
            auto   added = harvestLoaded(dimid, reg, (size_t)opt.loadedTarget - loadedCnt, snapshot, st);
            res.loadedAdded = added.size();
            res.loadedStats = st;
            if (!added.empty()) {
                std::unique_lock lk(mMutex);
                for (PreLanding const& p : added) addPointLocked(dimid, p);
            }
        } else {
            res.loadedStats.qualified = 0;
        }
    }

    // ── ④ 收尾：记区域 + 落盘 ──
    {
        std::unique_lock lk(mMutex);
        DimData&           d = mDims[dimid];
        d.present = true;
        d.originX = reg.originX;
        d.originZ = reg.originZ;
        d.radius  = reg.radius;
        d.builtAt = (int64_t)std::time(nullptr);
        rebuildIndexLocked(dimid);
    }
    save();
    return res;
}

std::string PreLandingPool::diagInfo() const {
    std::shared_lock lk(mMutex);
    std::string      s;
    char             buf[256];
    std::snprintf(buf, sizeof(buf), "%s seed=%lld mc=%d", mOpts.enabled ? "enabled" : "disabled",
                  (long long)mSeed, mMc);
    s = buf;
    for (int i = 0; i < 3; i++) {
        DimData const& d = mDims[i];
        if (!d.present && d.points.empty()) continue;
        size_t l = 0, u = 0, bad = 0;
        for (PreLanding const& p : d.points) {
            if (p.state == kPlStateInvalid) {
                bad++;
                continue;
            }
            if (p.tier == kPlTierLoaded) l++;
            else u++;
        }
        std::snprintf(buf, sizeof(buf), " | dim%d: 已加载 %zu/%d 未加载 %zu/%d 失效 %zu 半径 %d%s",
                      i, l, mOpts.loadedTarget, u, mOpts.unloadedTarget, bad, d.radius,
                      d.pending ? " (待重建)" : "");
        s += buf;
    }
    return s;
}

} // namespace mtps
