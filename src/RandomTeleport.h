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
    // 冷却（按预设各自记账, 键 = 玩家 + presetName）:
    //   >0 用该秒数; ==0 用全局 randomTeleport.cooldownSeconds; <0 不检查（调用方自带冷却）
    int         cooldownSeconds{0};
    std::string presetName{};         // 预设名（冷却记账用; 空则退回 message）
    std::string economyType{"default"};
};

// 随机传送（四级数据源 + 引擎 TickingArea 兜底版）
// 机制: 玩家原地等待 → 随机选坐标 → 判定落点 chunk（内存 → 落点预计算表 → 存档直读 →
// tickingarea 生成）→ 整 chunk 无安全列则按 chunk 粒度扩圈 → 换随机点重来(最多 6 次)
// → 失败退款, 玩家全程原地。数据源演进见 README「随机传送怎么生成新区块」。
// 常加载区域: 等生成时只挂小区域（半径小则中心区块更早轮到位）, 传送前再扩到宽限半径并保留
// RTP_AREA_GRACE_TICKS 才撤（立刻撤会让客户端收不到区块数据 → 灰屏）。
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

    // 传送成功后的"宽限期待撤区域"（会话已结束, 但区域还要留一会儿）
    struct GraceArea {
        std::string name;
        int         dim{-1};
        int64_t     removeAtTick{0};
        int         lcx{0}, lcz{0};   // 落点区块（自检: 撤区域后它是否仍被玩家视野持有）
        bool        removed{false};   // 区域是否已撤（撤完再等 20 tick 做一次自检）
    };
    std::vector<GraceArea> mGraceAreas;
    int64_t                mTickCounter{0};

    // 状态机返回值（原来用 0/1/2 靠注释解释）
    enum class StepResult { Progress, Done, Waiting };

    void pickNewTarget(Session& s);                    // 选新随机落点（不传送玩家）
    // 取一个"存档里已知安全"的已生成地块当落点; false = 存档里没有符合条件的
    bool tryKnownLanding(Session& s);
    // 一次选点失败后的收尾: 名额内换点重随 → 最后试一次已知安全点 → 仍不行才放弃
    StepResult retryOrGiveUp(Session& s, Player& p);

    // 会话推进：主函数只分发，各状态逻辑在自己的 stepXxx 里
    StepResult stepSession(Session& s);
    StepResult stepProbe(Session& s, Level& level, Player& p, Dimension& dim);
    StepResult stepLoadChunk(Session& s, Level& level, Player& p, Dimension& dim);
    StepResult stepScanChunk(Session& s, Level& level, Player& p, Dimension& dim);
    StepResult stepExpand(Session& s, Level& level, Player& p, Dimension& dim);
    void finishTeleport(Session& s, Player& p, bool success, SafePos const* pos);

    static void ensureTickingArea(Session& s, Level& level, int blockX, int blockZ, int radiusChunks);
    void cleanupSessionArea(Session& s);   // 非静态: 传送成功后要把区域挂到宽限期队列

    void scheduleAreaRemoval(std::string const& name, int dim, int delayTicks,
                             int landingCX, int landingCZ);
    void processGraceAreas();                          // 宽限期到 → 撤销区域
    static bool areaCoversLanding(Session const& s, SafePos const& p);  // 落点是否在区域覆盖内
};

} // namespace mtps
