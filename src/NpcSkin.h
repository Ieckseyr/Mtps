#pragma once
// NpcSkin.h - 假玩家 NPC 的皮肤来源管理（库的皮肤表纯内存, 消费方自己负责注册与持久化）。
// 来源优先级: Meowdata/Mtps/npc_skins/（<id>.png / <id>/ 子文件夹） > Config.skins.extraDirs
// （默认指向 MHR 的目录, 认 *.bin 快照） > 库里已注册的（与 MHR 共用同一个库实例）。
// skinId 为空 → 用 kDefaultId; 目录里没有就首次自动生成一张史蒂夫配色 PNG。
#include <string>
#include <vector>

namespace mtps {
namespace skins {

// 默认皮肤 ID（未指定皮肤时用它）
inline constexpr char const* kDefaultId = "default";

// 扫描所有来源并注册（插件启用时、/mtpsreload、菜单「重载皮肤」时调用）
// verbose=false 用于"顺手刷新"（进 NPC 管理页时）: 只注册不刷日志
// 返回当前可用的皮肤总数（含其他插件注册进来的）
int refresh(bool verbose = true);

// 保证某个皮肤可用: 已注册直接返回 true, 否则去目录里按需导入
bool ensure(std::string const& skinId);

// 解析出一个真的能用的皮肤 ID: 传入为空/不可用 → 默认皮肤; 连默认皮肤都不可用 → 空串
std::string resolve(std::string const& skinId);

// 当前可用的皮肤 ID（下拉框用; 默认皮肤在最前, 其余按字典序）
std::vector<std::string> available();

} // namespace skins
} // namespace mtps
