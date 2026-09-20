#pragma once
// TpUtil: 统一的玩家传送入口 —— 两种状态下不能传送, 都等它结束或提示稍后重试。
// 1) 出生状态机没跑完: 引擎随后仍会执行 ChooseSpawnPosition/SpawnComplete, 把玩家放回出生点并把
//    客户端的区块发布区域挂在出生点, 表现为"服务端传送成功、玩家眼前一直一片灰"且不自愈。
// 2) 跨维度切换中: 玩家的区块区域已被引擎挂起、客户端还在加载画面。这时传送会把玩家在两个维度
//    之间撕成两个实体 —— 出现幽灵玩家, 而且能刷物。必须等切换结束才允许传送。
// 异步动作(RTP)等这两件事结束再继续; 瞬发动作(传送点/TPA/召集/NPC)提示稍后重试。
#include <mc/deps/core/math/Vec3.h>
#include <mc/world/actor/player/Player.h>

#include <string>

namespace mtps {

// 出生流程是否已完成（对应日志里的 Player Spawned）
inline bool isPlayerSpawned(::Player const& player) { return player.mIsInitialSpawnDone; }

// 是否正在跨维度切换（区块区域被挂起, 或换维度请求已排队还没跑完, 或维度刚变、客户端还在加载）
bool isPlayerInDimensionTransfer(::Player const& player);

// 每 tick 调一次（Mtps 的 ServerLevelTickEvent）: 记录在线玩家的维度变化时刻
void tpGuardTick();

// 不能传送的原因文案（能传时返回空串）。调用方拼自己的前缀: "§c[传送] " + tpBlockedText(p)
std::string tpBlockedText(::Player const& player);

// 传送; 不能传时返回 false 且不做任何动作（调用方负责提示或稍后重试）
bool teleportPlayerIfReady(::Player& player, ::Vec3 const& pos, ::DimensionType dim);

} // namespace mtps
