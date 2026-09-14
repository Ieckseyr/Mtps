#pragma once
// TpUtil: 统一的玩家传送入口 —— 出生状态机跑完之前一律不传送。
// 否则引擎随后仍会执行 ChooseSpawnPosition/SpawnComplete, 把玩家放回出生点并把客户端的
// 区块发布区域挂在出生点, 表现为"服务端传送成功、玩家眼前一直一片灰"且不自愈。
// 异步动作(RTP)等出生完成再继续; 瞬发动作(传送点/TPA/召集/NPC)提示稍后重试。
#include <mc/deps/core/math/Vec3.h>
#include <mc/world/actor/player/Player.h>

namespace mtps {

// 出生流程是否已完成（对应日志里的 Player Spawned）
inline bool isPlayerSpawned(::Player const& player) { return player.mIsInitialSpawnDone; }

// 传送; 出生流程未完成时返回 false 且不做任何动作（调用方负责提示或稍后重试）
bool teleportPlayerIfReady(::Player& player, ::Vec3 const& pos, ::DimensionType dim);

} // namespace mtps
