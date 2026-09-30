// BiomeSampler.cpp - cubiomes-bedrock 的薄封装（见 BiomeSampler.h）
// 本文件不得引入任何 MC/LL 头（cubiomes 的 enum Dimension 与 BDS 头重定义冲突）。
#include "BiomeSampler.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

extern "C" {
#include "biomenoise.h"
#include "biomes.h"
#include "generator.h"
#include "util.h"
}

namespace mtps {

namespace {

// 线程局部生成器。Generator 含噪声行缓存且不保证并发安全（ZXDash 的 BiomeBackground.cpp 同样
// 按 thread_local 隔离），已核实它没有堆分配，所以直接按值放 thread_local。
// 目前只有两个线程会用到：主线程（传送路径的群系预筛）与 ArchiveScanner 的后台线程（建池）。
struct TlsGen {
    Generator    g{};
    SurfaceNoise sn{};
    int64_t      seed{INT64_MIN};
    int          mc{-1};
    bool         ok{false};
};

TlsGen& tls() {
    static thread_local TlsGen t;
    return t;
}

// 取当前线程的生成器，种子/mc 变了就原地重建
Generator* tlsGen(int64_t seed, int mc, SurfaceNoise** snOut) {
    TlsGen& t = tls();
    if (!t.ok || t.seed != seed || t.mc != mc) {
        setupGenerator(&t.g, mc, 0);
        applySeed(&t.g, DIM_OVERWORLD, (uint64_t)seed);
        initSurfaceNoise(&t.sn, DIM_OVERWORLD, (uint64_t)seed);
        t.seed = seed;
        t.mc   = mc;
        t.ok   = true;
    }
    if (snOut) *snOut = &t.sn;
    return &t.g;
}

// 方块坐标 → 群系格坐标（scale=4）。算术右移即负数向下取整，与瓦片渲染一致。
inline int cellOf(int block) { return block >> 2; }

// 与群系底图共用的采样切片：scale=4 的 y=15（≈方块 y60，近海平面）
constexpr int kSampleCellY = 15;

} // namespace

BiomeSampler& BiomeSampler::getInstance() {
    static BiomeSampler instance;
    return instance;
}

// 与 ZXDash plugins/ZXPanel/src/mod/BiomeBackground.cpp::mcForVersion 完全同源，改一边要同步改另一边
int BiomeSampler::mcForVersion(int major, int minor, int patch, std::string* verOut) {
    if (verOut) {
        *verOut = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }
    if (major <= 0) return MC_NEWEST;   // 检测失败 → 最新算法兜底

    // 版本三元组有两种形态，先归一成 BDS 新版方案（major=26, minor=40, patch=…）：
    //   A) 语义化 Minecraft 版本 (1, 26, 40)  B) BDS 内部版本 (26, 40, 8)
    if (major == 1 && minor >= 26) {
        major = minor;
        minor = patch;
        patch = 0;
    }
    if (major >= 26) {
        if (minor >= 40) return MC_26_40;
        if (minor >= 30) return MC_26_30;
        if (minor >= 20) return MC_26_20;
        return MC_1_21_60;              // 26.x 早期沿用 1.21.6x 群系树
    }
    if (major > 1) return MC_NEWEST;
    if (minor >= 21) {
        if (patch >= 60) return MC_1_21_60;
        if (patch >= 50) return MC_1_21_50;
        return MC_1_21_0;
    }
    if (minor == 20) return MC_1_20_0;
    if (minor == 19) return MC_1_19_0;
    if (minor == 18) return MC_1_18_0;
    if (minor == 17) return patch >= 30 ? MC_1_17_30 : MC_1_17_0;
    return MC_1_16_0;
}

bool BiomeSampler::configure(int64_t seed, int cubiomesMc,
                             std::vector<std::string> const& excludeNames, int neighborR) {
    if (cubiomesMc <= MC_UNDEF || cubiomesMc > MC_NEWEST) return false;

    // 群系 ID 只有 0..255，逐个名字比对建 ID 表。按名字而不是 ID：ID 会随分支/版本漂移。
    std::array<bool, 256> excl{};
    std::string           names;
    for (std::string const& want : excludeNames) {
        if (want.empty()) continue;
        bool hit = false;
        for (int id = 0; id < 256; id++) {
            const char* nm = biome2str(cubiomesMc, id);
            if (nm && std::strcmp(nm, want.c_str()) == 0) {
                excl[id] = true;
                hit      = true;
            }
        }
        if (!hit) continue;   // 这个名字在这个版本不存在（例如 legacy_frozen_ocean）
        if (!names.empty()) names += ",";
        names += want;
    }

    std::unique_lock lk(mMutex);
    mSeed         = seed;
    mMc           = cubiomesMc;
    mNeighborR    = std::max(0, std::min(8, neighborR));
    mExcluded     = excl;
    mExcludeNames = names;
    mReady        = true;
    return true;
}

bool BiomeSampler::ready() const {
    std::shared_lock lk(mMutex);
    return mReady;
}

int BiomeSampler::mc() const {
    std::shared_lock lk(mMutex);
    return mMc;
}

BiomeSampler::Sample BiomeSampler::sample(int blockX, int blockZ) const {
    Sample out;

    // 锁里只取策略快照，群系计算（~100µs）在锁外做
    int64_t               seed;
    int                   mc;
    int                   r;
    std::array<bool, 256> excl;
    {
        std::shared_lock lk(mMutex);
        if (!mReady) return out;
        seed = mSeed;
        mc   = mMc;
        r    = mNeighborR;
        excl = mExcluded;
    }

    SurfaceNoise* sn = nullptr;
    Generator*    g  = tlsGen(seed, mc, &sn);
    if (!g) return out;

    int const cx = cellOf(blockX);
    int const cz = cellOf(blockZ);
    int const w  = 2 * r + 1;

    // 一次覆盖 (2r+1)^2 群系格：中心与邻域同源，不会出现"中心走一条路径、邻域走另一条"的错位
    Range range = {4, cx - r, cz - r, w, w, kSampleCellY, 1};
    size_t need = getMinCacheSize(g, range.scale, range.sx, range.sy, range.sz);
    std::vector<int> ids(need > 0 ? need : 1, 0);
    if (genBiomes(g, ids.data(), range) != 0) return out;

    // 索引公式 cache[y*sx*sz + z*sx + x]（sy=1 → y=0），中心在 (x,z) = (r, r)
    out.biome = ids[(size_t)(r * w + r)];
    out.ok    = true;

    for (size_t i = 0; i < (size_t)w * (size_t)w; i++) {
        int const id = ids[i];
        if (id >= 0 && id < 256 && excl[(size_t)id]) {
            out.excluded = true;
            break;
        }
    }

    // 近似地表高度（1:4 采样，输出方块高度）。该版本/维度不走这条噪声路径时返回非 0，此时只是没有提示。
    float h  = 0.0f;
    int   rc = mapApproxHeight(&h, nullptr, g, sn, cx, cz, 1, 1);
    if (rc == 0) {
        out.approxY    = h;
        out.hasApproxY = true;
    }
    return out;
}

std::string BiomeSampler::diagInfo() const {
    std::shared_lock lk(mMutex);
    if (!mReady) return "not-configured";
    char buf[256];
    std::snprintf(buf, sizeof(buf), "seed=%lld mc=MC%d 邻域半径=%d格 排除群系=[%s]",
                  (long long)mSeed, mMc, mNeighborR, mExcludeNames.c_str());
    return std::string(buf);
}

} // namespace mtps
