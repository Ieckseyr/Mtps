#pragma once

#include "DataTypes.h"
#include <string>
#include <vector>
#include <memory>

class Level;
class Player;
class Dimension;

namespace mtps {

// 随机传送安全位置
struct SafePos {
    double x, y, z;
    int    dimid;
};

struct RtpOptions {
    int         dimid{0};
    double      originX{0};
    double      originZ{0};
    int         radius{1000};
    std::string originMode{"fixed"};  // fixed / player
    std::string message;
    int         cost{0};
    int         cooldownSeconds{0};   // 0 = 用全局配置
    std::string economyType{"default"};
};

// 随机传送（四级数据源 + 区块视野生成兜底版）
// 机制: 玩家原地等待 → 随机选坐标 → 判定落点 chunk（内存 → 落点预计算表 → 存档直读 →
// 区块视野生成）→ 整 chunk 无安全列则按 chunk 粒度扩圈(1~24) → 换随机点重来(最多 6 次)
// → 失败退款, 玩家全程原地。数据源演进见 README「设计说明」。
// 区块视野: 传送成功后留 RTP_VIEW_GRACE_TICKS 宽限期再释放（立刻释放会让客户端收不到区块
// 数据 → 灰屏）; 落点在区域覆盖范围外时先补一个以落点为中心的区域。
class RandomTeleport {
public:
    static RandomTeleport& getInstance();

    // 发起随机传送（同玩家旧会话自动作废）
    void start(Player& player, RtpOptions const& opts);

    // 每 tick 推进（由主入口调用）
    void tick();

    // 停止全部会话（玩家全程在原地, 无需回退传送; 常加载区域一并移除）
    void stopAll();

private:
    RandomTeleport() = default;

    struct Session;
    std::vector<std::shared_ptr<Session>> mSessions;

    // 传送成功后的"宽限期待保持区块"（会话已结束, 但落点区块还要周期性再请求一会儿,
    // 等玩家客户端拿到区块、自己的视野接管）
    struct GraceArea {
        int     dim{-1};
        int     lcx{0}, lcz{0};      // 落点区块（宽限期内周期性再请求, 结束后做一次自检）
        int64_t removeAtTick{0};     // 宽限期结束时刻
        int64_t nextPumpTick{0};     // 下次再请求的时刻
        bool    removed{false};      // 是否已停止请求（停止后再等 20 tick 做自检）
    };
    std::vector<GraceArea> mGraceAreas;
    int64_t                mTickCounter{0};

    // 状态机返回值（原来用 0/1/2 靠注释解释）
    enum class StepResult { Progress, Done, Waiting };

    void pickNewTarget(Session& s);                    // 选新随机落点（不传送玩家）

    // 会话推进：主函数只分发，各状态逻辑在自己的 stepXxx 里
    StepResult stepSession(Session& s);
    StepResult stepProbe(Session& s, Level& level, Player& p, Dimension& dim);
    StepResult stepLoadChunk(Session& s, Level& level, Player& p, Dimension& dim);
    StepResult stepScanChunk(Session& s, Level& level, Player& p, Dimension& dim);
    StepResult stepExpand(Session& s, Level& level, Player& p, Dimension& dim);
    void finishTeleport(Session& s, Player& p, bool success, SafePos const* pos);

    void requestChunkLoad(Session& s, int blockX, int blockZ);   // 向引擎请求加载/生成该区块（用到 mTickCounter, 非静态）
    void cleanupSessionArea(Session& s);   // 非静态: 传送成功后要把落点区块挂到宽限期队列

    void scheduleKeepAlive(int dim, int lcx, int lcz, int delayTicks);
    void processGraceAreas();                          // 宽限期内周期性再请求, 到期自检
};

} // namespace mtps
