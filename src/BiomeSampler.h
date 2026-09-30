#pragma once
// 按世界种子推算群系与近似地表高度（cubiomes-bedrock）。
// 与 ZXDash 的群系底图同源：同一个库（third_party/cubiomes，从 ZXPanel 整目录拷贝）、同一个种子
// （Level::getLevelSeed64 的完整 64 位）、同一个版本映射、同一个采样切片（scale=4 群系格 + y=15
// ≈ 方块 y60），所以"这里判定的群系"和"地图上看到的底图"是同一个答案。
// 注意：BiomeSampler.cpp 不得引入任何 MC/LL 头（cubiomes 的 enum Dimension 与 BDS 头重定义冲突），
// 所以这个头也不暴露 cubiomes 类型，生成器状态全藏在 .cpp 的 thread_local 里。
#include <array>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

namespace mtps {

class BiomeSampler {
public:
    static BiomeSampler& getInstance();

    // 启动/配置重载时调用，幂等。seed 传完整 64 位，cubiomesMc 用 mcForVersion 从 BDS 版本换算。
    // 返回 false = cubiomes 版本非法，此时 sample() 一律 ok=false，上层走降级路径。
    bool configure(int64_t seed, int cubiomesMc,
                   std::vector<std::string> const& excludeNames, int neighborR);

    bool ready() const;
    int  mc() const;

    struct Sample {
        int   biome{-1};         // 中心群系格的群系 ID
        bool  excluded{false};   // 中心或邻域出现被排除的群系
        float approxY{0.0f};     // 近似地表高度（方块），只作提示/预筛，不是真实地形
        bool  hasApproxY{false};
        bool  ok{false};         // 采样失败或未配置
    };

    // 一次采样：中心群系 + 邻域排除判定 + 近似高度（实测约 90µs）
    Sample sample(int blockX, int blockZ) const;

    // BDS 版本三元组 → cubiomes 算法版本（与 ZXDash 的 BiomeBackground::mcForVersion 同一套，
    // 两份实现要同步改）
    static int mcForVersion(int major, int minor, int patch, std::string* verOut = nullptr);

    std::string diagInfo() const;

private:
    BiomeSampler() = default;
    BiomeSampler(BiomeSampler const&)            = delete;
    BiomeSampler& operator=(BiomeSampler const&) = delete;

    mutable std::shared_mutex mMutex;
    bool                      mReady{false};
    int64_t                   mSeed{0};
    int                       mMc{-1};
    int                       mNeighborR{2};
    std::array<bool, 256>     mExcluded{};    // 由名字解析出来的群系 ID 排除表
    std::string               mExcludeNames;  // 给 diagInfo 用
};

} // namespace mtps
