// AdminMenu.cpp - 管理员参数设置表单（读配置 → 编辑 → 写回 Config.json）。
// 与 Menu.cpp 的传送点数据管理分开, 免得两个文件都膨胀;
// 写入语义: 每次修改立即 buildCaches() + 落盘, 改完即生效。
#include "Config.h"
#include "DataStore.h"
#include "Menu.h"
#include "MenuCommon.h"

#include <ll/api/form/CustomForm.h>
#include <ll/api/form/SimpleForm.h>

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mtps {
namespace menu {

namespace {

// 费用项里目前真正被强制执行的只有这三项（其余键仅为配置兼容保留, 界面上标注出来）
bool isCostEnforced(std::string const& key) {
    return key == "publicWarp" || key == "privateWarp" || key == "tpa";
}

// 单行文本输入表单（经济名称等）
void openTextInput(Player& player, std::string const& title, std::string const& label,
                   std::string const& current, std::string const& fieldKey) {
    ll::form::CustomForm fm("§l§e" + title);
    fm.appendInput("value", label, current, current);
    fm.sendTo(player, [fieldKey](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) return openEconomySettingsForm(pl);
        auto&       cfg = Config::getInstance();
        std::string cur = (fieldKey == "moneyName") ? cfg.moneyName() : cfg.scoreboardName();
        std::string v   = formGetString(res, "value", cur);
        // 去首尾空白
        size_t b = v.find_first_not_of(" \t\r\n");
        size_t e = v.find_last_not_of(" \t\r\n");
        v = (b == std::string::npos) ? std::string{} : v.substr(b, e - b + 1);
        if (v.empty()) {
            tell(pl, "§c名称不能为空");
            return openEconomySettingsForm(pl);
        }
        if (cfg.setEconomyText(fieldKey, v)) {
            tell(pl, "§a已设置为 §e" + v);
        } else {
            tell(pl, "§c保存失败（配置目录不可写？）");
        }
        openEconomySettingsForm(pl);
    });
}

// 各项费用（每个动作的单价）
void openEconomyCostForm(Player& player) {
    auto& cfg = Config::getInstance();

    ll::form::CustomForm fm("§l§6设置各项传送费用");
    for (auto& [key, name] : Config::costKeys()) {
        std::string label = "§e" + name;
        if (!isCostEnforced(key)) label += "§8(暂未生效)";
        fm.appendSlider(key, label, 0, 10000, 10, cfg.getCost(key));
    }
    fm.sendTo(player, [](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) return openEconomySettingsForm(pl);
        auto& c = Config::getInstance();
        int   n = 0;
        for (auto& [key, name] : Config::costKeys()) {
            int v = (int)formGetNumber(res, key, (double)c.getCost(key));
            if (c.setEconomyCost(key, v)) n++;
        }
        tell(pl, "§a已更新 §e" + std::to_string(n) + " §a项费用");
        openEconomyCostForm(pl);
    });
}

} // namespace

// 传送点设置: 私人传送点上限
void openWarpLimitSettingsForm(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能访问系统设置！");
        return openMainMenu(player);
    }
    auto& cfg = Config::getInstance();

    ll::form::CustomForm fm("§l§e传送点设置");
    fm.appendSlider("max", "§e私人传送点最大数量", 1, 50, 1, cfg.maxPrivateWarps());
    fm.appendLabel("§7提示: 修改后对所有玩家生效, 已有传送点不会被删除");
    fm.sendTo(player, [](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) return openAdminSettingsMenu(pl);
        auto& cfg = Config::getInstance();
        int   n   = (int)formGetNumber(res, "max", (double)cfg.maxPrivateWarps());
        if (cfg.setMaxPrivateWarps(n)) {
            tell(pl, "§a私人传送点上限已设置为 §e" + std::to_string(n) + "§a 个");
        } else {
            tell(pl, "§c保存失败（配置目录不可写？）");
        }
        openWarpLimitSettingsForm(pl);
    });
}

// 传送参数设置: 冷却时间 / 请求有效时长
void openTeleportParamsForm(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能访问系统设置！");
        return openMainMenu(player);
    }
    auto& cfg = Config::getInstance();

    ll::form::CustomForm fm("§l§b传送参数设置");
    fm.appendSlider("cooldown", "§e传送冷却时间（秒）", 0, 300, 5, cfg.cooldownTime());
    fm.appendSlider("duration", "§e请求有效时长（秒）", 10, 300, 5, cfg.effectiveDuration());
    fm.appendLabel("§8注: 冷却时间目前仅作为配置项保存, 尚未强制拦截（与 JS 版一致）");
    fm.sendTo(player, [](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) return openAdminSettingsMenu(pl);
        auto& cfg = Config::getInstance();
        int   cd  = (int)formGetNumber(res, "cooldown", (double)cfg.cooldownTime());
        int   du  = (int)formGetNumber(res, "duration", (double)cfg.effectiveDuration());
        bool  ok  = cfg.setTeleportParams(cd, du);
        if (ok) {
            tell(pl, "§a传送参数已更新\n§7冷却时间: §e" + std::to_string(cd) +
                     "秒§7, 有效时长: §e" + std::to_string(du) + "秒");
        } else {
            tell(pl, "§c保存失败（配置目录不可写？）");
        }
        openTeleportParamsForm(pl);
    });
}

// 经济系统设置
void openEconomySettingsForm(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能访问系统设置！");
        return openMainMenu(player);
    }
    auto& cfg = Config::getInstance();

    std::string content = "§r当前经济系统设置:\n";
    content += "§7启用状态: " + std::string(cfg.economyEnabled() ? "§a已启用" : "§c已禁用") + "\n";
    content += "§7经济类型: §e" + cfg.economyType() + "\n";
    content += "§7计分板名称: §e" + cfg.scoreboardName() + "\n";
    content += "§7货币名称: §e" + cfg.moneyName() + "\n";
    content += "§7私人传送费用: §e" + std::to_string(cfg.getCost("privateWarp"));

    ll::form::SimpleForm fm("§l§a经济系统设置", content);

    fm.appendButton(std::string(cfg.economyEnabled() ? "§c[×] 禁用经济系统" : "§a[√] 启用经济系统") +
                        "\n§7点击切换",
                    tex::INFO, "path", [&player](Player&) {
                        auto& cfg = Config::getInstance();
                        bool  on  = !cfg.economyEnabled();
                        if (cfg.setEconomyEnabled(on)) {
                            tell(player, std::string("§a经济系统已") + (on ? "启用" : "禁用"));
                        } else {
                            tell(player, "§c保存失败");
                        }
                        openEconomySettingsForm(player);
                    });
    fm.appendButton("§e切换经济类型\n§7当前: " + cfg.economyType() + "（llmoney / scoreboard）",
                    tex::SETTINGS, "path", [&player](Player&) {
                        auto&       cfg  = Config::getInstance();
                        std::string next = (cfg.economyType() == "llmoney") ? "scoreboard" : "llmoney";
                        if (cfg.setEconomyType(next)) {
                            tell(player, "§a经济类型已切换为 §e" + next);
                        } else {
                            tell(player, "§c保存失败");
                        }
                        openEconomySettingsForm(player);
                    });
    fm.appendButton("§b设置计分板名称\n§7当前: " + cfg.scoreboardName(), tex::INFO, "path",
                    [&player](Player&) {
                        auto& cfg = Config::getInstance();
                        openTextInput(player, "设置计分板名称", "§e计分板名称", cfg.scoreboardName(),
                                      "scoreboardName");
                    });
    fm.appendButton("§d设置货币名称\n§7当前: " + cfg.moneyName(), tex::INFO, "path",
                    [&player](Player&) {
                        auto& cfg = Config::getInstance();
                        openTextInput(player, "设置货币名称", "§e货币名称", cfg.moneyName(), "moneyName");
                    });
    fm.appendButton("§6设置各项传送费用\n§7按动作分别设置单价", tex::SETTINGS, "path",
                    [&player](Player&) { openEconomyCostForm(player); });
    fm.appendButton("§7返回", tex::BACK, "path", [&player](Player&) { openAdminSettingsMenu(player); });

    fm.sendTo(player);
}

// 默认规则设置（新玩家默认规则）
void openDefaultRulesSettingsForm(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能访问系统设置！");
        return openMainMenu(player);
    }
    auto& cfg = Config::getInstance();

    std::string content = "§r新玩家默认规则:\n";
    content += "§7拒绝所有TPA: " + std::string(cfg.defaultRefuseAllTpa() ? "§c已启用" : "§a已禁用") + "\n";
    content += "§7关闭弹窗提示: " + std::string(cfg.defaultNoPopUpWindow() ? "§c已启用" : "§a已禁用") + "\n";
    content += "§7拒绝传送点申请: " + std::string(cfg.defaultRefuseAllWarpRequests() ? "§c已启用" : "§a已禁用");

    ll::form::SimpleForm fm("§l§d默认规则设置", content);

    fm.appendButton(std::string(cfg.defaultRefuseAllTpa() ? "§c[×] 关闭" : "§a[√] 启用") +
                        " 拒绝所有TPA\n§7点击切换",
                    tex::REFUSE, "path", [&player](Player&) {
                        auto& cfg = Config::getInstance();
                        bool  on  = !cfg.defaultRefuseAllTpa();
                        if (!cfg.setDefaultRule("refuseAllTpaRequest", on)) tell(player, "§c保存失败");
                        else tell(player, std::string("§a默认拒绝TPA已") + (on ? "启用" : "禁用"));
                        openDefaultRulesSettingsForm(player);
                    });
    fm.appendButton(std::string(cfg.defaultNoPopUpWindow() ? "§c[×] 关闭" : "§a[√] 启用") +
                        " 关闭弹窗提示\n§7点击切换",
                    tex::INFO, "path", [&player](Player&) {
                        auto& cfg = Config::getInstance();
                        bool  on  = !cfg.defaultNoPopUpWindow();
                        if (!cfg.setDefaultRule("noPopUpWindow", on)) tell(player, "§c保存失败");
                        else tell(player, std::string("§a默认关闭弹窗已") + (on ? "启用" : "禁用"));
                        openDefaultRulesSettingsForm(player);
                    });
    fm.appendButton(std::string(cfg.defaultRefuseAllWarpRequests() ? "§c[×] 关闭" : "§a[√] 启用") +
                        " 拒绝传送点申请\n§7点击切换",
                    tex::WARP, "path", [&player](Player&) {
                        auto& cfg = Config::getInstance();
                        bool  on  = !cfg.defaultRefuseAllWarpRequests();
                        if (!cfg.setDefaultRule("refuseAllWarpRequests", on)) tell(player, "§c保存失败");
                        else tell(player, std::string("§a默认拒绝传送点申请已") + (on ? "启用" : "禁用"));
                        openDefaultRulesSettingsForm(player);
                    });
    fm.appendButton("§7返回", tex::BACK, "path", [&player](Player&) { openAdminSettingsMenu(player); });

    fm.sendTo(player);
}

// 重载配置（带确认）
void openReloadConfigForm(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能访问系统设置！");
        return openMainMenu(player);
    }
    ll::form::SimpleForm fm("§l§c重载配置",
                            "§r确定要从文件重新加载配置吗？\n\n"
                            "§a热重载范围:\n§7- 重新读取 Config.json\n§7- 立即生效所有更改\n\n"
                            "§c警告: 尚未落盘的界面修改将会丢失！");

    fm.appendButton("§a确认重载\n§7从文件加载", tex::ACCEPT, "path", [&player](Player&) {
        if (Config::getInstance().reload()) {
            tell(player, "§a配置已热重载！所有更改已立即生效");
        } else {
            tell(player, "§c配置重载失败（已回退默认值, 请检查文件格式）");
        }
        openAdminSettingsMenu(player);
    });
    fm.appendButton("§c取消\n§7返回设置", tex::CANCEL, "path",
                    [&player](Player&) { openAdminSettingsMenu(player); });

    fm.sendTo(player);
}

// 系统管理菜单: 参数设置入口 + 数据管理入口
void openAdminSettingsMenu(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能访问系统设置！");
        return openMainMenu(player);
    }

    auto& cfg = Config::getInstance();
    auto& ds  = DataStore::getInstance();

    ll::form::SimpleForm fm("§l§4系统管理",
        "§r当前系统配置:" + std::string("\n") +
        "§7私人传送点上限: §e" + std::to_string(cfg.maxPrivateWarps()) + "§r 个" + std::string("\n") +
        "§7请求有效时长: §e" + std::to_string(cfg.effectiveDuration()) + "§r 秒" + std::string("\n") +
        "§7经济系统: " + (cfg.economyEnabled() ? "§a已启用" : "§c已禁用") + std::string("\n") +
        "§7数据: 公共点 §e" + std::to_string(ds.getPublicWarps().size()) +
        "§7 | NPC点 §e" + std::to_string(ds.getBlockTpPoints().size()) + "§7 个");

    // 参数设置
    fm.appendButton("§e传送点设置" + std::string("\n") + "§7私人传送点数量上限", tex::WARP, "path",
        [](Player& p) { openWarpLimitSettingsForm(p); });
    fm.appendButton("§b传送参数设置" + std::string("\n") + "§7冷却时间、请求有效时长", tex::SETTINGS, "path",
        [](Player& p) { openTeleportParamsForm(p); });
    fm.appendButton("§a经济系统设置" + std::string("\n") + "§7开关、类型、名称、各项费用", tex::INFO, "path",
        [](Player& p) { openEconomySettingsForm(p); });
    fm.appendButton("§d默认规则设置" + std::string("\n") + "§7新玩家默认规则", tex::PLAYER, "path",
        [](Player& p) { openDefaultRulesSettingsForm(p); });

    // 数据管理
    fm.appendButton("§b搜索玩家 / 传送点" + std::string("\n") + "§7可搜到私人传送点并直接修改", tex::INFO, "path",
        [](Player& p) { openWarpSearchForm(p); });
    fm.appendButton("§4管理公共传送点" + std::string("\n") + "§7增删、修改坐标", tex::SETTINGS, "path",
        [](Player& p) { openPublicWarpManageMenu(p); });
    if (cfg.blockTpEnabled()) {
        fm.appendButton("§4NPC传送点管理" + std::string("\n") + "§7创建/管理NPC传送点", tex::SETTINGS, "path",
            [](Player& p) { openBlockTpMenu(p); });
    }
    fm.appendButton("§c重载配置" + std::string("\n") + "§7从文件重新加载（带确认）", tex::BACK, "path",
        [](Player& p) { openReloadConfigForm(p); });
    fm.appendButton("§7返回主菜单", tex::BACK, "path", [](Player& p) { openMainMenu(p); });

    fm.sendTo(player);
}

} // namespace menu
} // namespace mtps
