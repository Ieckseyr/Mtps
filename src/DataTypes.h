#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace mtps {

// 维度 ID
constexpr int DIM_OVERWORLD = 0;
constexpr int DIM_NETHER    = 1;
constexpr int DIM_END       = 2;

inline const char* dimName(int dim) {
    switch (dim) {
        case DIM_OVERWORLD: return "主世界";
        case DIM_NETHER:    return "下界";
        case DIM_END:       return "末地";
        default:            return "未知";
    }
}

// 传送点位置
struct WarpPos {
    double x{0};
    double y{0};
    double z{0};
    int    dimid{0};
};

// 经济配置
struct EconomyEntry {
    bool        enabled{false};
    std::string type{"llmoney"};      // llmoney / scoreboard / default
    std::string scoreboardName{"money"};
    std::string moneyName{"金币"};
    int         cost{0};
};

// 私人传送点
struct PrivateWarp {
    std::string name;
    WarpPos     pos;
    int64_t     createdAt{0};
    bool        noApproval{false};   // 免申请
    EconomyEntry economy;
};

// 公共传送点
struct PublicWarp {
    std::string name;
    WarpPos     pos;
    std::string owner;
    bool        enabled{true};
    EconomyEntry economy;
};

// 免申请传送点（从私人传送点中标记）
// 实际存储在 PrivateWarps 中，noApproval=true

// 随机传送预设
struct RandomPreset {
    std::string name;
    bool        enabled{true};
    std::string originMode{"fixed"};  // fixed / player
    double      originX{0};
    double      originZ{0};
    int         radius{1000};
    int         dimid{0};
    std::string message;
    int         cooldown{0};   // 该预设单独的冷却（秒; 0 = 用全局 randomTeleport.cooldownSeconds）
    EconomyEntry economy;
};

// btp 传送点（NPC 传送点）
struct BlockTpPoint {
    std::string name;
    bool        enabled{true};
    WarpPos     trigger;           // NPC 站立位置
    bool        isRandom{false};
    WarpPos     target;            // 固定传送目标
    // 随机传送目标
    double      randomOriginX{0};
    double      randomOriginZ{0};
    // 随机中心模式: fixed = 用上面记录的固定中心; entity = 用触发实体(NPC/实体)自身当前位置
    std::string randomOriginMode{"fixed"};
    int         randomRadius{1000};
    int         randomDimid{0};
    std::string message;
    std::vector<std::string> itemFilter;
    // 悬浮字
    bool        floatingTextEnabled{true};
    std::string floatingText;
    float       floatingOffsetY{2.0f};
    EconomyEntry economy;
    // 冷却（秒; 0 = 不限制）, 每个点可单独设
    int         cooldownSeconds{0};
    // 实体交互与表现开关（仅 carrier == 1 的虚假实体生效）
    bool        lookAtPlayers{true};      // 逐客户端看向自己（每个观察者都看到"它正看着我"）
    bool        triggerOnInteract{true};  // 右键交互触发传送
    bool        triggerOnAttack{true};    // 左键攻击触发传送
    // 载体类型: 0 = 假玩家 NPC（HologramLib IPlayerNpc）
    //          1 = 虚假实体（HologramLib ICustomEntity; 纯协议生成, 不占服务端实体系统）
    // 下面这几个字段两种载体共用 —— 字段名不带载体前缀, 免得"明明是实体却叫 npcXxx"。
    int         carrier{0};
    std::string entityType{"minecraft:armor_stand"};  // 仅 carrier == 1: 实体标识符（短名自动补 minecraft:）
    std::string skinId;                              // 仅 carrier == 0: NPC 皮肤（npc_skins 里的名字）
    std::string displayName;                         // 名字牌文本（NPC 名字牌 / 实体 nametag 同一字段）
    float       yaw{0};                              // 朝向（度, Bedrock 约定: 0=南 90=西）
    float       scale{1.0f};                         // 模型缩放
    // 落点高度微调（格, 可负; 0 = 脚正好站在触发点）。两种载体各一个: 客户端对 AddActor 与
    // AddPlayer 的 y 原点未必一致, 用它对齐（两个设同值 = 整体上下平移）。
    float       npcHeightOffset{0.0f};      // 假玩家 NPC（carrier == 0）
    float       entityHeightOffset{0.0f};   // 虚假实体（carrier == 1）
    // 世界坐标微调（格）: 载体与悬浮字一起平移, 用来把点位挪到精确位置。
    // 竖直方向不在这里 —— 上面两个"高度微调"按载体分, 两个设成同值就等于一个共用的 Y
    // （高度微调还负责对齐模型原点, 所以比共用的那个更强, 不再重复放一个 Y）。
    float       posOffsetX{0.0f};
    float       posOffsetZ{0.0f};
};

// TPA 请求
struct TpaRequest {
    std::string fromName;
    std::string toName;
    std::string type;          // tpa / tpahere
    int64_t     timestamp{0};
    int         duration{60};
    std::string fromServer;    // 跨服用
};

// 传送点申请请求
struct WarpRequest {
    std::string requester;
    std::string targetPlayer;
    std::string warpName;
    WarpPos     pos;
    int64_t     timestamp{0};
    int         duration{60};
};

// 跨服传送目的地（点一下就转到另一个服务器, 走客户端的 transfer 机制）
struct CrossServerEntry {
    std::string name;                 // 显示名（支持颜色码）
    std::string address;              // 服务器地址（域名或 IP）
    int         port{19132};
    bool        enabled{true};
};

// 个人规则
struct PersonalRules {
    bool refuseAllTpaRequest{false};
    bool noPopUpWindow{false};
    bool refuseAllWarpRequests{false};
    // 黑名单: 名单里的玩家不能向我发起任何传送（互传/申请都会被挡）
    std::vector<std::string> blockedPlayers;
};

} // namespace mtps
