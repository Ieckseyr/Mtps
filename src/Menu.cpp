#include "Menu.h"
#include "Config.h"
#include "DataStore.h"
#include "Economy.h"
#include "RandomTeleport.h"
#include "WarpManager.h"
#include "TpaRally.h"
#include "NpcTeleport.h"
#include "DataTypes.h"
#include "MenuCommon.h"
#include "TpUtil.h"
#include "Pinyin.h"

#include <hologramlib/HologramLib.h>

#include <ll/api/form/SimpleForm.h>
#include <ll/api/form/CustomForm.h>
#include <ll/api/service/Bedrock.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/actor/player/PlayerListEntry.h>
#include <mc/world/level/Level.h>
#include <mc/deps/core/math/Vec3.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace mtps {
namespace menu {

// 图标常量与 isOp/tell 已移到 MenuCommon.h（AdminMenu.cpp 也要用）

// 建立 xuid → 玩家名 映射（一次 O(N); 用 Level::getPlayerList() 才能带上离线玩家的名字,
// 原实现逐个 forEachPlayer 是乘积级开销且只认在线玩家）。
// 主菜单
void openMainMenu(Player& player) {
    bool const isop = isOp(player);

    auto& cfg = Config::getInstance();

    int onlineCount = 0;
    auto level = ll::service::getLevel();
    if (level) {
        level->forEachPlayer([&](Player&) -> bool { onlineCount++; return true; });
    }

    ll::form::SimpleForm fm("§l§d传送系统",
        "§r在线玩家: §e" + std::to_string(onlineCount) + "§r 人\n§7选择功能开始使用");

    if (cfg.isFeatureEnabled("warpSystem")) {
        fm.appendButton("§a传送点系统\n§7快捷传送到指定位置", tex::WARP, "path",
            [&player](Player&) { openWarpMenu(player); });
    }

    if (cfg.isFeatureEnabled("tpa")) {
        fm.appendButton("§b玩家互传\n§7发起传送请求", tex::PLAYER, "path",
            [&player](Player&) { openTpaSelectForm(player); });
    }
    if (cfg.isFeatureEnabled("randomTeleport") && cfg.randomTeleportEnabled()
        && !cfg.randomPresets().empty()) {
        fm.appendButton("§b随机传送\n§7传送到随机安全位置", tex::WARP, "path",
            [&player](Player&) { openRandomTeleportMenu(player); });
    }
    fm.appendButton("§6请求管理\n§7查看待处理的请求", tex::REQUEST, "path",
        [&player](Player&) { openRequestManageForm(player); });
    fm.appendButton("§e个人设置\n§7配置传送偏好", tex::SETTINGS, "path",
        [&player](Player&) { openPersonalSettingsForm(player); });
    fm.appendButton(std::string("§c黑名单管理\n§7名单里的玩家不能向你发起传送"), tex::BLACKLIST, "path",
        [&player](Player&) { openBlacklistForm(player); });

    if (isop) {
        fm.appendButton("§4系统管理\n§7[管理员] 配置系统参数", tex::SETTINGS, "path",
            [&player](Player&) { openAdminSettingsMenu(player); });
    }

    fm.sendTo(player);
}

// 传送点系统
static std::string describePreset(RandomPreset const& p) {
    std::string dimText = dimName(p.dimid);
    std::string originText = (p.originMode == "player")   ? "以玩家为原点"
                           : (p.originMode == "entity") ? "以触发实体为原点"
                                                        : "固定原点";
    return dimText + " §7| §f" + originText + " §7| §f半径 §e" + std::to_string(p.radius);
}

void openRandomTeleportMenu(Player& player, std::string const& actionStr) {
    auto& cfg = Config::getInstance();

    // 启用中的预设
    std::vector<RandomPreset> presets;
    for (auto& p : cfg.randomPresets()) {
        if (p.enabled) presets.push_back(p);
    }

    if (presets.empty()) {
        tell(player, "§c暂无随机传送预设！");
        return openWarpMenu(player);
    }

    // 编号直达（指令名见 Config.commands.random）
    if (!actionStr.empty()) {
        int num = 0;
        try { num = std::stoi(actionStr); } catch (...) { num = 0; }
        if (num >= 1 && num <= (int)presets.size()) {
            auto& preset = presets[num - 1];
            int cost = preset.economy.enabled ? preset.economy.cost : 0;
            if (cost > 0 && cfg.economyEnabled() && !Economy::getInstance().canAfford(player, cost)) {
                tell(player, "§c余额不足！需要 " + formatCost(cost));
                return;
            }
            RtpOptions opts;
            opts.dimid = preset.dimid;
            opts.originMode = preset.originMode;
            opts.originX = preset.originX;
            opts.originZ = preset.originZ;
            opts.radius = preset.radius;
            opts.message = preset.message;
            opts.cost = cost;
            opts.presetName      = preset.name;
            opts.cooldownSeconds = preset.cooldown;   // 该预设自己的冷却（0 = 用全局）
            RandomTeleport::getInstance().start(player, opts);
            return;
        }
        tell(player, "§c[随机传送] §f无效编号 §e" + actionStr + "§f，可用范围 §a1~" +
             std::to_string(presets.size()) + "§f（§e/" + cfg.getCommand("random") + " 编号§f 直达）");
        return;
    }

    ll::form::SimpleForm fm("§l§b随机传送", "§r选择预设传送到随机安全位置");

    for (auto& preset : presets) {
        std::string costText;
        if (preset.economy.enabled && preset.economy.cost > 0) {
            costText = "\n§7消耗: " + formatCost(preset.economy.cost);
        }
        fm.appendButton("§e" + preset.name + "\n§7" + describePreset(preset) + costText, tex::WARP, "path",
            [&player, preset](Player&) {
                auto& cfg2 = Config::getInstance();
                int cost = preset.economy.enabled ? preset.economy.cost : 0;
                if (cost > 0 && cfg2.economyEnabled() && !Economy::getInstance().canAfford(player, cost)) {
                    tell(player, "§c余额不足！需要 " + formatCost(cost));
                    return;
                }
                RtpOptions opts;
                opts.dimid = preset.dimid;
                opts.originMode = preset.originMode;
                opts.originX = preset.originX;
                opts.originZ = preset.originZ;
                opts.radius = preset.radius;
                opts.message = preset.message;
                opts.cost = cost;
                opts.presetName      = preset.name;
                opts.cooldownSeconds = preset.cooldown;
                RandomTeleport::getInstance().start(player, opts);
            });
    }
    if (isOp(player)) {
        fm.appendButton("§4[管理] 预设设置\n§7增删改预设: 名称/原点/半径/维度/冷却/费用", tex::SETTINGS, "path",
            [&player](Player&) { openRtpPresetList(player); });
    }
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openMainMenu(player); });

    fm.sendTo(player);
}

// 随机传送预设管理（管理员）: 存在 Config.json 的 randomTeleport.presets, 改完即落盘。

void openRtpPresetList(Player& player);

// idx < 0 = 新建, 否则编辑第 idx 个
void openRtpPresetEdit(Player& player, int idx) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能改随机传送预设！");
        return openMainMenu(player);
    }
    auto& cfg     = Config::getInstance();
    auto  presets = cfg.randomPresets();
    if (idx >= (int)presets.size()) return openRtpPresetList(player);

    RandomPreset      p;
    bool const        isNew = (idx < 0);
    if (!isNew) p = presets[idx];

    ll::form::CustomForm fm(isNew ? "§l§a新建随机传送预设" : "§l§e编辑预设: " + p.name);
    fm.appendInput("name", "§e名称", p.name, p.name);
    fm.appendStepSlider("originMode", "§e原点", {"固定坐标", "以发起玩家为原点"},
                        (size_t)(p.originMode == "player" ? 1 : 0));
    fm.appendInput("originX", "§e原点 X（固定坐标时生效）", std::to_string((int)p.originX),
                   std::to_string((int)p.originX));
    fm.appendInput("originZ", "§e原点 Z（固定坐标时生效）", std::to_string((int)p.originZ),
                   std::to_string((int)p.originZ));
    fm.appendInput("radius", "§e半径（格）", std::to_string(p.radius), std::to_string(p.radius));
    fm.appendStepSlider("dimid", "§e维度", {"主世界", "下界", "末地"}, (size_t)std::clamp(p.dimid, 0, 2));
    fm.appendInput("cooldown", "§e冷却（秒, 0 = 用全局）", std::to_string(p.cooldown),
                   std::to_string(p.cooldown));
    fm.appendInput("cost", "§e费用（0 = 免费）", std::to_string(p.economy.cost),
                   std::to_string(p.economy.cost));
    fm.appendInput("message", "§e传送消息", p.message.empty() ? "§a[随机传送] §f已传送！" : p.message,
                   p.message.empty() ? "§a[随机传送] §f已传送！" : p.message);
    fm.sendTo(player, [idx, isNew](Player& pl, ll::form::CustomFormResult const& res,
                                   ll::form::FormCancelReason) {
        if (!res.has_value()) return openRtpPresetList(pl);
        auto& c   = Config::getInstance();
        auto  all = c.randomPresets();

        RandomPreset np;
        np.name       = formGetString(res, "name");
        if (np.name.empty()) {
            tell(pl, "§c名称不能为空");
            return openRtpPresetList(pl);
        }
        int const om   = formGetStep(res, "originMode", {"固定坐标", "以发起玩家为原点"}, 0);
        np.originMode  = (om == 1) ? "player" : "fixed";
        np.originX     = formGetNumber(res, "originX", 0);
        np.originZ     = formGetNumber(res, "originZ", 0);
        np.radius      = std::max(16, (int)formGetNumber(res, "radius", 1000));
        np.dimid       = formGetStep(res, "dimid", {"主世界", "下界", "末地"}, 0);
        np.cooldown    = std::max(0, (int)formGetNumber(res, "cooldown", 0));
        np.enabled     = true;
        np.message     = formGetString(res, "message");
        np.economy.enabled = false;   // 走下面 cost 即可
        np.economy.cost    = std::max(0, (int)formGetNumber(res, "cost", 0));
        np.economy.type    = "default";

        if (isNew) {
            all.push_back(np);
        } else {
            all[idx] = np;
        }
        if (c.setPresets(all)) {
            tell(pl, isNew ? ("§a已新建预设 §e" + np.name) : "§a已保存预设");
        } else {
            tell(pl, "§c保存失败");
        }
        openRtpPresetList(pl);
    });
}

void openRtpPresetList(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能改随机传送预设！");
        return openMainMenu(player);
    }
    auto const& presets = Config::getInstance().randomPresets();

    ll::form::SimpleForm fm("§l§e随机传送预设",
                            "§r共 §e" + std::to_string(presets.size()) + "§r 个预设\n"
                            "§7点击某个预设可以改名称/原点/半径/维度/冷却/费用/消息");
    fm.appendButton("§a新建预设\n§7添加一个随机传送预设", tex::ADD, "path",
                    [](Player& p) { openRtpPresetEdit(p, -1); });
    for (size_t i = 0; i < presets.size(); i++) {
        auto const& p = presets[i];
        std::string info = std::string(dimName(p.dimid)) + " §7| §f" +
                           std::string(p.originMode == "player" ? "以玩家为原点" : "固定原点") +
                           " §7| §f半径 §e" + std::to_string(p.radius);
        if (p.cooldown > 0) info += " §7| §f冷却 §e" + std::to_string(p.cooldown) + "s";
        if (p.economy.cost > 0) info += " §7| §f费用 §e" + std::to_string(p.economy.cost);
        fm.appendButton((p.enabled ? "§e" : "§8[禁用] ") + p.name + "\n§7" + info, tex::SETTINGS, "path",
                        [i](Player& pl) { openRtpPresetEdit(pl, (int)i); });
        {
            std::string pname = p.name;
            fm.appendButton("§c[删除] " + pname, tex::DELETE, "path", [pname](Player& pl) {
                auto  c   = Config::getInstance().randomPresets();
                auto  out = std::vector<RandomPreset>{};
                for (auto& x : c) {
                    if (x.name != pname) out.push_back(x);
                }
                if (out.size() == c.size()) {
                    tell(pl, "§c没有找到该预设");
                } else if (Config::getInstance().setPresets(out) && !out.empty()) {
                    tell(pl, "§c已删除预设 §e" + pname);
                } else {
                    // 全删光会让人以为功能坏了: 留一个默认预设
                    std::vector<RandomPreset> keep;
                    RandomPreset              d;
                    d.name    = "主世界随机";
                    d.radius  = 1000;
                    d.dimid   = 0;
                    d.message = "§a[随机传送] §f已传送到随机位置！";
                    keep.push_back(d);
                    Config::getInstance().setPresets(keep);
                    tell(pl, "§c已删除预设 §e" + pname + "§c（已自动补回一个默认预设）");
                }
                openRtpPresetList(pl);
            });
        }
    }
    fm.appendButton("§7返回", tex::BACK, "path", [](Player& p) { openRandomTeleportMenu(p); });
    fm.sendTo(player);
}

void openBlacklistForm(Player& player) {
    auto&        ds      = DataStore::getInstance();
    std::string const me = player.getRealName();
    auto         blocked = ds.getBlockedPlayers(me);

    std::string content = "§r当前黑名单:\n§c";
    if (blocked.empty()) {
        content += "暂无";
    } else {
        for (size_t i = 0; i < blocked.size(); i++) {
            content += blocked[i] + (i + 1 < blocked.size() ? ", " : "");
        }
    }

    std::vector<std::string> online;
    if (auto level = ll::service::getLevel()) {
        level->forEachPlayer([&](Player& p) -> bool {
            if (p.getRealName() != me) online.push_back(p.getRealName());
            return true;
        });
    }

    ll::form::CustomForm fm("§l§c黑名单管理");
    fm.appendLabel(content);
    if (online.empty()) {
        fm.appendLabel("§7当前没有其他在线玩家");
        fm.sendTo(player, [](Player& pl, ll::form::CustomFormResult const&, ll::form::FormCancelReason) {
            openMainMenu(pl);
        });
        return;
    }
    fm.appendDropdown("target", "§e选择玩家", online, 0);
    fm.appendStepSlider("op", "§e操作", {"加入黑名单", "移出黑名单"}, 0);
    fm.appendToggle("confirm", "§a确认修改（不勾选不执行）", false);

    fm.sendTo(player, [online](Player& pl, ll::form::CustomFormResult const& res,
                               ll::form::FormCancelReason) {
        if (!res.has_value()) return openMainMenu(pl);
        if (!formGetBool(res, "confirm", false)) {
            tell(pl, "§c未勾选确认，操作已取消");
            return openMainMenu(pl);
        }
        int const idx = formGetStep(res, "target", online, 0);
        if (idx < 0 || idx >= (int)online.size()) return openMainMenu(pl);
        std::string const target = online[idx];
        int const         op     = formGetStep(res, "op", {"加入黑名单", "移出黑名单"}, 0);

        auto&             ds2  = DataStore::getInstance();
        std::string const me2  = pl.getRealName();
        auto              list = ds2.getBlockedPlayers(me2);
        auto              it   = std::find(list.begin(), list.end(), target);
        if (op == 0) {
            if (it != list.end()) {
                tell(pl, "§c" + target + " 已在黑名单中");
            } else {
                list.push_back(target);
                ds2.setBlockedPlayers(me2, list);
                tell(pl, "§a已将 §e" + target + "§a 加入黑名单");
            }
            return openBlacklistForm(pl);
        }
        if (it == list.end()) {
            tell(pl, "§c" + target + " 不在黑名单中");
            return openBlacklistForm(pl);
        }
        list.erase(it);
        ds2.setBlockedPlayers(me2, list);
        tell(pl, "§a已将 §e" + target + "§a 移出黑名单");
        openBlacklistForm(pl);
    });
}

void openPersonalSettingsForm(Player& player) {
    auto rules = DataStore::getInstance().getPersonalRules(player.getRealName());

    ll::form::CustomForm fm("§l§e个人设置");
    fm.appendToggle("refuseTpa", "§e拒绝所有传送请求", rules.refuseAllTpaRequest);
    fm.appendToggle("noPopup", "§e关闭弹窗通知", rules.noPopUpWindow);

    fm.sendTo(player, [](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) return;
        PersonalRules rules = DataStore::getInstance().getPersonalRules(pl.getRealName());
        auto it = res->find("refuseTpa");
        if (it != res->end()) {
            if (auto* vu = std::get_if<uint64>(&it->second)) rules.refuseAllTpaRequest = (*vu != 0);
            else if (auto* vd = std::get_if<double>(&it->second)) rules.refuseAllTpaRequest = (*vd != 0.0);
        }
        it = res->find("noPopup");
        if (it != res->end()) {
            // 分支内变量换个名字, 免得和上面那个 v 互相遮蔽（编译器会报 C4456）
            if (auto* vu = std::get_if<uint64>(&it->second)) rules.noPopUpWindow = (*vu != 0);
            else if (auto* vd = std::get_if<double>(&it->second)) rules.noPopUpWindow = (*vd != 0.0);
        }
        DataStore::getInstance().setPersonalRules(pl.getRealName(), rules);
        tell(pl, "§a个人设置已保存");
    });
}

// 召集传送
} // namespace menu
} // namespace mtps
