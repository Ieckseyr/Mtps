#pragma once

#include "DataTypes.h"
#include <string>
#include <unordered_map>
#include <memory>

class Player;

namespace mtps {

// 假玩家 NPC / 虚假实体 两种载体的统一管理。两种载体在库里是两个管理器、接口签名也不同
// （朝向 NPC 没有 pitch、名字牌字段名不同）—— 这些差异只允许出现在 carrierApi() 适配层里;
// 上层只认「载体 id + isEntity」。
class NpcTeleport {
public:
    static NpcTeleport& getInstance();

    // 初始化：从 DataStore 加载所有 btp 点并创建载体 + 悬浮字
    void init();

    // 只刷新单个传送点的载体/悬浮字（增删改后调用）
    void refreshPoint(std::string const& pointName);

    // 每 tick 驱动: 逐客户端朝向 + 载体 id 失配自愈（10Hz 节流）
    void tick();
    // 朝向开关变化后立即生效（关闭"看向玩家"时清除逐客户端朝向覆盖）
    void applyLookMode(std::string const& pointName);

    // 玩家与载体交互（ghost 事件; domain = "npc" / "entity"）
    //   action: InteractPacket Action 原始值（1=右键交互 2=左键攻击 4=交互更新）
    void onInteract(std::string const& domain, std::string const& playerName, int64_t id, int action);

    // 重新加载全部载体（配置/数据变化时）
    void reloadAll();

private:
    NpcTeleport() = default;

    struct NpcRuntime {
        int64_t     carrierId{-1};    // 库内载体 id（两种载体共用一张映射表）
        bool        isEntity{false};  // true = 虚假实体, false = 假玩家 NPC
        int64_t     mhrUid{-1};       // 托管给 MHR 时的 uid（移除时用）
        bool        mhrOwned{false};
        int64_t     holoId{-1};
        int         staleTicks{0};    // 记录里的载体 id 连续多少轮对不上（自愈用）
        std::string pointName;
    };

    std::unordered_map<std::string, NpcRuntime> mNpcs;  // pointName -> runtime
    // 载体 id -> 点位名。两种载体的 id 段互不相交（NPC 自增段从 1 起, 虚假实体固定在
    // [0x10000000,0x7FFFFFFF)）, 所以一张表就够 —— 这也是能统一路由的前提。
    std::unordered_map<int64_t, std::string> mIdToPoint;
    // 每个玩家在每个点上的上次传送时刻（键 = 玩家名 + 点位名）
    std::unordered_map<std::string, int64_t> mLastUse;
    // 交互防抖: 每个玩家在每个点上的上次交互时刻（单调时钟毫秒; 键同 mLastUse）
    std::unordered_map<std::string, int64_t> mLastInteractMs;

    // 唯一创建入口（内部按载体类型分派, 收尾登记统一走 registerRuntime）
    void createCarrierForPoint(BlockTpPoint const& point);
    void createNpcCarrier(BlockTpPoint const& point, NpcRuntime& rt);     // 填 rt.carrierId
    void createEntityCarrier(BlockTpPoint const& point, NpcRuntime& rt);  // 填 rt.carrierId / mhrUid / mhrOwned
    void    registerRuntime(BlockTpPoint const& point, NpcRuntime rt);
    void    createHologramForPoint(BlockTpPoint const& point, NpcRuntime& rt);
    void    destroyHologramOf(std::string const& pointName);   // 重建前收掉旧悬浮字（防客户端叠字）

    // 载体 id 重新认领: 库内 id 不保证稳定（经 MHR 改过属性后会变）, 靠名字牌认回来
    int64_t     currentCarrierIdOf(BlockTpPoint const& point) const;
    std::string pointNameByCarrierId(bool isEntity, int64_t id) const;
    void        adoptCarrierId(std::string const& pointName, NpcRuntime& rt, int64_t newId);

    void destroyCarrier(std::string const& pointName);
    void destroyAllNpcs();

    void teleportPlayerToPoint(Player& player, BlockTpPoint const& point);
    // 交互公共逻辑（两种载体共用）
    void handleInteract(std::string const& playerName, std::string const& pointName, int action);
    // 同一玩家 + 同一点在防抖窗口内的重复交互 → true（丢弃本次事件）
    bool interactDebounced(std::string const& key);
};

} // namespace mtps
