#pragma once

#include <string>

class Player;

namespace mtps {

namespace menu {

// 主菜单
void openMainMenu(Player& player);

// 传送点系统
void openWarpMenu(Player& player);

// 跨服传送（列表 / 管理员增删改）
void openCrossServerList(Player& player);

// 随机传送菜单（actionStr 非空 = 编号直达, 指令名见 Config.commands.random）
void openRandomTeleportMenu(Player& player, std::string const& actionStr = "");

// 随机传送预设管理（管理员）
void openRtpPresetList(Player& player);

// 我的传送点（actionStr 非空 = 编号直达, 指令名见 Config.commands.private）
void openPrivateWarpMenu(Player& player, std::string const& actionStr = "");

// 管理我的传送点（免申请开关、删除、重设坐标到当前位置[新增]）
void openPrivateWarpManageMenu(Player& player);

// 公共传送点列表（actionStr 非空 = 编号直达, 指令名见 Config.commands.public）
void openPublicWarpQuickList(Player& player, std::string const& actionStr = "");

// 管理公共传送点（管理员: 添加、删除、修改坐标到当前位置[新增]）
void openPublicWarpManageMenu(Player& player);

// 免申请传送点列表
void openNoApprovalWarpsList(Player& player, std::string const& actionStr = "");

// 玩家传送点浏览（浏览他人共享点）
void openPublicWarpBrowser(Player& player);

// 模糊搜索玩家 / 传送点（含"所有玩家传送点"列表）
void openWarpSearchForm(Player& player);

// NPC 传送点管理（原 btp，管理员）
void openBlockTpMenu(Player& player);

// 单个 NPC 传送点的管理表单（管理员; 手持编辑物品/蹲下右键 NPC·实体 时直接进这里）
void openBlockTpDetail(Player& player, std::string const& pointName);

// 系统管理（管理员）
void openAdminSettingsMenu(Player& player);

// 管理员参数设置（对齐 JS 版的 5 个管理表单）
void openWarpLimitSettingsForm(Player& player);      // 私人传送点上限
void openTeleportParamsForm(Player& player);         // 冷却时间 / 请求有效时长
void openEconomySettingsForm(Player& player);        // 经济开关 / 类型 / 名称 / 各项费用
void openDefaultRulesSettingsForm(Player& player);   // 新玩家默认规则
void openReloadConfigForm(Player& player);           // 重载配置（带确认）

// [新增] 查看所有玩家的传送点（管理员）
void openAllPlayerWarpsMenu(Player& player);

// [新增] 某玩家的传送点详情（管理员可传送）
void openPlayerWarpDetail(Player& player, std::string const& xuid);

// TPA 玩家互传
void openTpaSelectForm(Player& player);

// 请求管理
void openRequestManageForm(Player& player);

// 个人设置
void openPersonalSettingsForm(Player& player);

// 黑名单管理（名单里的玩家不能向我发起传送）
void openBlacklistForm(Player& player);

// 发起召集（再执行一次 = 取消）
void startRally(Player& player);

// 同意/拒绝传送请求（命令用; 同意也用于响应召集）
void acceptTeleportRequest(Player& player);
void refuseTeleportRequest(Player& player);

} // namespace menu

} // namespace mtps
