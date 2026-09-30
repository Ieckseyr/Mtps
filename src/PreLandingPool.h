#pragma once
// 随机传送的预落点池：候选落点预先算好并落盘，传送时只做一次内存查询（设计与实测见
// docs/MTPS随机传送预落点优化方案.md）。
// 两类点，分界是"存档里有没有这块地"（这个分界稳定、可持久化）：
//   已加载类 = 落点表来的已生成点，传送只需把区块载入内存
//   未加载类 = cubiomes 按种子推的候选点，传送要生成地形（唯一允许的等待）
// 几条别破坏的约束：
//   1. pick() 不做存档 IO、不做批量群系计算；重活都在 topUp() 里，且 topUp() 只由后台线程调
//   2. approxY 只当提示/预筛，绝不作为传送落点的 Y——真实 Y 永远由生成后的地表扫描决定
//   3. 池没就绪/为空/没命中就回落到原有四级数据源，功能不受影响，只是没有快路径
#include <cstdint>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtps {

// 一个预落点（POD，直接按字节落盘，改字段要同步升文件版本号）
struct PreLanding {
    int32_t  x{0}, z{0};    // 方块坐标
    int32_t  approxY{0};    // 未加载类: cubiomes 推算高度; 已加载类: 落点表给的真实站立 Y
    uint32_t lastUsed{0};   // 最近一次被命中（unix 秒，淘汰用）
    uint16_t biome{0};      // 采样到的群系 ID（诊断）
    uint8_t  tier{0};       // kPlTierLoaded / kPlTierUnloaded
    uint8_t  state{0};      // kPlStateXxx
};
inline constexpr uint8_t kPlTierLoaded      = 0;
inline constexpr uint8_t kPlTierUnloaded    = 1;
inline constexpr uint8_t kPlStateCandidate  = 0;   // 候选（还没被真实扫描验证过）
inline constexpr uint8_t kPlStateVerified   = 1;   // 已验证（落点表来的，或生成后扫描通过）
inline constexpr uint8_t kPlStateInvalid    = 2;   // 失效（传送失败/无安全列），等补齐替换

// 池参数，由 Config 组装后传进来（池不直接读配置字段名）
struct PoolOptions {
    bool   enabled{true};
    int    loadedTarget{1000};
    int    unloadedTarget{3000};
    int    bucketSize{512};         // 空间桶边长（格）
    int    minSeparation{64};       // 池内点最小间距（格），防扎堆；只作用于未加载类的生成
    int    maxTriesPerPoint{64};
    int    approxHeightMin{63};     // 低于此近似高度的候选点丢弃（≈海平面）
    double replaceRatio{0.5};       // 半径变更时替换掉旧未加载点的比例
    bool   rebuildOnRadiusChange{true};
    std::string file{"landingpool.bin"};
};

class PreLandingPool {
public:
    static PreLandingPool& getInstance();

    void        setOptions(PoolOptions const& o);
    PoolOptions options() const;

    // 指纹（世界种子 / cubiomes 版本），必须在 load() 之前设。与盘上 meta 不符就整体作废重建。
    void setSeedInfo(int64_t seed, int cubiomesMc);

    bool load();
    bool save() const;
    void unload();

    // 生成区域：每个维度取该维度上半径最大的启用预设
    struct Region {
        int    dimid{0};
        double originX{0}, originZ{0};
        int    radius{0};
        bool   valid{false};
    };
    void   setRegions(std::vector<Region> const& regions);
    Region regionOf(int dimid) const;

    bool   usable() const;
    size_t count(int dimid, int tier, bool onlyUsable) const;

    struct Pick {
        int x{0}, z{0};              // 池里的点本身（回写状态用这个坐标）
        int targetX{0}, targetZ{0};  // 实际传送目标，就是池点（见 pick）
        int hintY{0};                // 参考高度，只看日志
        int tier{0};
        int biome{0};
    };
    // 抽一个可用预落点。avoidX/avoidZ/avoidR 是同一玩家上次落点与最小间隔（avoidR<=0 不约束），
    // 有约束时优先挑离它够远的点，都在附近才退回普通的等概率抽。
    // wantTier 只抽指定类（预热器用它专抽未加载类），-1 = 不限。
    bool pick(int dimid, double originX, double originZ, int radius, uint64_t seed,
              int avoidX, int avoidZ, int avoidR, Pick& out, int wantTier = -1);

    // 常驻点：后台已生成好、区块还留在内存里的点。抽取时优先用它们，落上去是瞬时的。
    void setResident(std::vector<PreLanding> const& pts);
    // 预热器取一个值得预热的未加载类点：池里随机一个、且不在刚预热过的名单里
    bool takeWarmCandidate(int dimid, double originX, double originZ, int radius, uint64_t seed,
                           std::vector<std::pair<int, int>> const& recent, Pick& out);

    void markVerified(int dimid, int x, int z, int realY);   // 未加载类 → 已加载类（自愈升级）
    void markInvalid(int dimid, int x, int z);               // → 失效，下次补齐替换

    // 后台补齐（只由后台线程调用）。已加载类取点的统计口径：
    //   examined 本次考察的存档落点数，already 其中已在池里的，qualified 其中新合格能入池的
    struct HarvestStats { size_t examined{0}, already{0}, qualified{0}; };

    struct TopUpResult {
        size_t loadedAdded{0};
        size_t unloadedAdded{0};
        size_t unloadedReplaced{0};       // 半径重建时替换掉的旧点
        size_t evicted{0};                // 超出目标的淘汰
        size_t dropped{0};                // 失效点清理
        size_t outOfRegion{0};            // 区域外被清掉的已加载类点（半径缩小后出现）
        HarvestStats loadedStats{};
        size_t effectiveUnloadedTarget{0};// 受半径容量限制后的实际目标
        bool   rebuilt{false};
        bool   ok{false};
    };
    TopUpResult topUp(int dimid);

    bool rebuildPending(int dimid) const;

    std::string diagInfo() const;

private:
    PreLandingPool() = default;
    PreLandingPool(PreLandingPool const&)            = delete;
    PreLandingPool& operator=(PreLandingPool const&) = delete;

    struct DimData {
        std::vector<PreLanding> points;
        // 空间桶索引，只在内存里：bucketKey → points 下标数组
        std::unordered_map<int64_t, std::vector<uint32_t>> buckets;
        std::vector<int64_t>                               nonEmpty;
        bool        present{false};      // 盘上存过这一维度
        double      originX{0}, originZ{0};
        int         radius{0};           // 建池时用的半径（0 = 没建过）
        bool        pending{false};
        double      pendingOriginX{0}, pendingOriginZ{0};
        int         pendingRadius{0};
        int64_t     builtAt{0};
    };

    static int64_t bucketKey(int bx, int bz);
    void  rebuildIndexLocked(int dimid);                      // 需持有排他锁
    void  addPointLocked(int dimid, PreLanding const& p);     // 需持有排他锁
    std::vector<int64_t> candidateBucketsLocked(int dimid, double ox, double oz, int radius) const;

    bool saveMeta() const;
    bool loadMeta();

    // 未加载类：cubiomes 生成一批候选点（重活，锁外执行）
    std::vector<PreLanding> generateUnloaded(int dimid, Region const& region, size_t want,
                                             std::vector<PreLanding> const& avoid);
    // 已加载类：从落点表快照里等距采样（重活，锁外执行）。
    // 这里刻意不加 minSeparation：落点表本身就是"每 chunk 一个点"（间距 16 格），再套 64 格间距
    // 会把九成合格点滤掉（实测 856 → 110）。最小间距只服务于未加载类那种自己撒点的场景。
    std::vector<PreLanding> harvestLoaded(int dimid, Region const& region, size_t want,
                                         std::vector<PreLanding> const& avoid, HarvestStats& st);

    mutable std::shared_mutex mMutex;
    PoolOptions               mOpts;
    DimData                   mDims[3];
    int64_t                   mSeed{0};   // 建池时的种子（指纹）
    int                       mMc{-1};    // 建池时的 cubiomes 版本（指纹）
    bool                      mLoaded{false};
    // 盘上池的取面规则版本和本进程不一致（比如后来加了"树冠不算地表"），已加载类的 approxY
    // 就过时了，要重收；未加载类不受影响（那批点是 cubiomes 推的）
    bool                      mTier0Stale{false};
    std::vector<PreLanding>   mResident;  // 常驻点，抽取时优先
};

} // namespace mtps
