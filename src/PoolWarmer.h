#pragma once
// 预落点池的后台预热：把"待生成"的预落点逐个生成出来并验证。
// 玩家落到未加载类的点上时引擎要**现场**生成那一圈 5x5 区块（实测 1.4~5.8 秒，光照占一半），
// 预热把这份工作挪到后台慢慢做：热一个点，池里就多一个"已生成且已验证"的点，传送只需加载；
// 常驻的那些区块留在内存里，落上去是瞬时的。
// 不是"预生成整张地图"：预设半径只是抽样范围，半径 500000 的圆盘约 30.7 亿个区块，物理上不可能
// 整片生成。池里那 3000 个点本身就是对该半径的均匀抽样，逐个热过去等价于"大致覆盖到这个范围"。
// 速率与让路：常驻位不满时每 5 秒补一个，满了回到 pool.warm.intervalSeconds 的慢节奏；
// 有玩家正在传送时自动让路；池里没有可热的点时长冷却后重试。
#include <cstdint>
#include <string>
#include <vector>

#include "PreLandingPool.h"

class Level;

namespace mtps {

class PoolWarmer {
public:
    static PoolWarmer& getInstance();

    void start();   // 启用（幂等）
    void stop();    // 停用：撤掉占用的常加载区域
    void tick();    // 每 tick 推进一次（主入口在 tick 事件里调，区块加载/扫描必须在主线程）

    // 玩家用到过这个点（传送结束回写时调）→ 释放常驻位给下一个预热出来的点。
    // 必须显式通知：预热本身就会把池里的状态从"候选"改成"已验证"，靠比对状态位判断
    // "被用过"会把刚热好的点立刻误判掉（我第一版就是这么写错的）。
    void noteUsed(int dim, int x, int z);

private:
    PoolWarmer() = default;
    PoolWarmer(PoolWarmer const&)            = delete;
    PoolWarmer& operator=(PoolWarmer const&) = delete;

    struct Spot {
        PreLanding  p;
        std::string area;
        int         dim{0};
    };

    void startWarm();                            // 选点 + 挂区域
    bool attachWarmArea(Level& level);
    void finishWarm(bool ok, int realY);         // 收尾：回写状态 + 常驻管理
    void releaseSpot(Spot& s);
    void refreshResidentList();                  // 同步给池（抽取时优先）
    void releaseArea(std::string const& name, int dim);

    bool     mRunning{false};
    int      mPhase{0};            // 0 = 等冷却, 1 = 等区块就绪
    int64_t  mCooldown{0};
    int      mWaitTicks{0};
    int      mDim{0};
    int      mX{0}, mZ{0};         // 本次预热的点
    std::string mArea;             // 本次预热挂的区域名
    int      mAreaCX{0}, mAreaCZ{0};

    std::vector<Spot> mResident;   // 常驻点（区块保持加载 → 瞬时传送）

    uint64_t mWarmed{0}, mVerified{0}, mNoSafe{0}, mAborted{0};
    int      mLastTicks{0};
    int      mLastRealY{0};
};

} // namespace mtps
