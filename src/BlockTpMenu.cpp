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
#include "NpcSkin.h"
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
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>

namespace mtps {
namespace menu {

void openBlockTpDetail(Player& player, std::string const& pointName);  // 见下方定义

void openBlockTpMenu(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能访问！");
        return openMainMenu(player);
    }
    // 顺手扫一遍皮肤目录: 刚丢进 npc_skins 的皮肤不用手动点「重载皮肤」也能在下拉框里看到
    skins::refresh(false);

    auto& ds = DataStore::getInstance();
    auto const& points = ds.getBlockTpPoints();

    // 编辑工具提示（配置里可关/可换物品）
    auto& cfgRef = Config::getInstance();
    std::string editHint;
    if (cfgRef.editToolEnabled()) {
        std::vector<std::string> ways;
        if (!cfgRef.editToolItem().empty()) {
            ways.push_back("手持 §f" + cfgRef.editToolItem() +
                           (cfgRef.editToolItemRequiresSneak() ? "§7并蹲下" : ""));
        }
        if (cfgRef.editToolSneak()) ways.push_back("§7蹲下");
        if (!ways.empty()) {
            editHint = "\n§7编辑: " + ways[0];
            for (size_t i = 1; i < ways.size(); ++i) editHint += " §7或 " + ways[i];
            editHint += "§7 右键 NPC/实体 = 直接打开该点管理表单";
        }
    }

    ll::form::SimpleForm fm("§l§4NPC传送点管理",
        "§r共有 §e" + std::to_string(points.size()) + "§r 个NPC传送点\n"
        "§7载体可选 §f假玩家NPC§7 或 §f虚假实体§7（实体类型可填）\n"
        "§7定点传送目标可 §f填坐标§7 或 §f选公共传送点" + editHint);

    // 添加固定传送点
    fm.appendButton("§a添加固定传送NPC\n§7在当前位置创建, 传到指定目标", tex::ADD, "path",
        [&player](Player&) {
            auto pos = player.getPosition();

            // 目标来源: 选一个公共传送点, 或填坐标
            std::vector<std::string> pubNames{"§7不使用（按下面的坐标）"};
            for (auto& w : DataStore::getInstance().getPublicWarps()) pubNames.push_back(w.name);

            ll::form::CustomForm form("§l§a添加固定传送NPC");
            form.appendInput("name", "§eNPC传送点名称", "输入名称");
            form.appendInput("displayName", "§e显示名称", "NPC名");
            form.appendStepSlider("carrier", "§e载体类型", {"假玩家NPC", "虚假实体(可填类型)"}, 0);
            form.appendInput("entityType", "§e实体类型（仅虚假实体）", "minecraft:armor_stand",
                             "minecraft:armor_stand");
            std::vector<std::string> skinIds = skins::available();
            if (skinIds.empty()) skinIds.push_back(skins::kDefaultId);
            form.appendDropdown("skin", "§e皮肤（仅假玩家NPC; 名单来自 npc_skins 目录）", skinIds, 0);
            form.appendDropdown("pubWarp", "§e目标: 选择公共传送点（优先于下面坐标）", pubNames, 0);
            form.appendInput("tx", "§e目标 X", std::to_string((int)pos.x), std::to_string((int)pos.x));
            form.appendInput("ty", "§e目标 Y", std::to_string((int)pos.y), std::to_string((int)pos.y));
            form.appendInput("tz", "§e目标 Z", std::to_string((int)pos.z), std::to_string((int)pos.z));
            form.appendStepSlider("tdim", "§e目标维度", {"主世界", "下界", "末地"},
                                  (size_t)std::clamp((int)player.getDimensionId(), 0, 2));
            form.appendInput("message", "§e传送消息", "§a[传送] 已传送！");
            form.appendToggle("floatingTextEnabled", "§e常显悬浮字", true);
            form.appendSlider("textY", "§e悬浮字高度（格）", 0, 12, 0.5, 2);
            form.appendInput("floatingText", "§e悬浮字内容", "点击NPC传送");
            form.sendTo(player, [pubNames, skinIds](Player& pl, ll::form::CustomFormResult const& res,
                                   ll::form::FormCancelReason) {
                if (!res.has_value()) return openBlockTpMenu(pl);
                std::string name = formGetString(res, "name");
                if (name.empty()) {
                    tell(pl, "§c名称不能为空");
                    return openBlockTpMenu(pl);
                }

                BlockTpPoint point;
                point.name    = name;
                point.enabled = true;
                auto pos      = pl.getPosition();
                point.trigger.x = pos.x; point.trigger.y = pos.y; point.trigger.z = pos.z;
                point.trigger.dimid = (int)pl.getDimensionId();
                point.isRandom = false;

                // 载体与实体类型
                point.carrier = ((int)formGetStep(res, "carrier", {"假玩家NPC", "虚假实体(可填类型)"}, 0) >= 1) ? 1 : 0;
                {
                    std::string et = formGetString(res, "entityType", "minecraft:armor_stand");
                    point.entityType = et.empty() ? "minecraft:armor_stand" : et;
                }

                // 目标: 优先用选中的公共传送点, 否则用填的坐标
                int pubIdx = formGetStep(res, "pubWarp", pubNames, 0);
                if (pubIdx > 0) {
                    auto const& pubs = DataStore::getInstance().getPublicWarps();
                    if (pubIdx - 1 < (int)pubs.size()) {
                        point.target = pubs[pubIdx - 1].pos;
                    } else {
                        point.target = point.trigger;
                    }
                } else {
                    point.target.x = formGetNumber(res, "tx", pos.x);
                    point.target.y = formGetNumber(res, "ty", pos.y);
                    point.target.z = formGetNumber(res, "tz", pos.z);
                    point.target.dimid = (int)formGetStep(res, "tdim", {"主世界", "下界", "末地"}, (int)pl.getDimensionId());
                }

                point.displayName = decodeNl(formGetString(res, "displayName"));
                if (point.displayName.empty()) point.displayName = name;
                point.message = formGetString(res, "message");
                if (point.message.empty()) point.message = "§a[传送] §f已传送！";
                point.floatingTextEnabled = formGetBool(res, "floatingTextEnabled", true);
                point.floatingText = decodeNl(formGetString(res, "floatingText"));
                point.floatingOffsetY = (float)formGetNumber(res, "textY", 2.0);
                if (point.floatingText.empty()) point.floatingText = "点击NPC传送";
                int const skinIdx = formGetStep(res, "skin", skinIds, 0);
                point.skinId   = (skinIdx >= 0 && skinIdx < (int)skinIds.size()) ? skinIds[skinIdx]
                                                                                : std::string(skins::kDefaultId);
                point.scale = 1.0f;

                if (DataStore::getInstance().addBlockTpPoint(point)) {
                    NpcTeleport::getInstance().refreshPoint(point.name);
                    tell(pl, "§a已创建NPC传送点 §e" + name);
                } else {
                    tell(pl, "§c创建失败（名称重复）");
                }
                openBlockTpMenu(pl);
            });
        });

    // 添加随机传送点
    fm.appendButton("§a添加随机传送NPC\n§7在当前位置创建, 传到随机位置", tex::ADD, "path",
        [&player](Player&) {
            ll::form::CustomForm form("§l§a添加随机传送NPC");
            form.appendInput("name", "§eNPC传送点名称", "输入名称");
            form.appendInput("displayName", "§e显示名称", "NPC名");
            form.appendStepSlider("carrier", "§e载体类型", {"假玩家NPC", "虚假实体(可填类型)"}, 0);
            form.appendInput("entityType", "§e实体类型（仅虚假实体）", "minecraft:armor_stand",
                             "minecraft:armor_stand");
            std::vector<std::string> skinIds = skins::available();
            if (skinIds.empty()) skinIds.push_back(skins::kDefaultId);
            form.appendDropdown("skin", "§e皮肤（仅假玩家NPC; 名单来自 npc_skins 目录）", skinIds, 0);
            form.appendInput("radius", "§e随机半径（格, 直接填数字）", "1000", "1000");
            form.appendStepSlider("originMode", "§e随机中心",
                                 {"世界中心(0,0)", "创建时NPC所在位置", "触发玩家所在位置"}, 0);
            form.appendInput("message", "§e传送消息", "§a[随机传送] 已传送！");
            form.appendToggle("floatingTextEnabled", "§e常显悬浮字", true);
            form.appendSlider("textY", "§e悬浮字高度（格）", 0, 12, 0.5, 2);
            form.appendInput("floatingText", "§e悬浮字内容", "点击NPC随机传送");
            form.sendTo(player, [skinIds](Player& pl, ll::form::CustomFormResult const& res,
                                   ll::form::FormCancelReason) {
                if (!res.has_value()) return openBlockTpMenu(pl);
                std::string name = formGetString(res, "name");
                if (name.empty()) {
                    tell(pl, "§c名称不能为空");
                    return openBlockTpMenu(pl);
                }

                BlockTpPoint point;
                point.name    = name;
                point.enabled = true;
                auto pos      = pl.getPosition();
                point.trigger.x = pos.x; point.trigger.y = pos.y; point.trigger.z = pos.z;
                point.trigger.dimid = (int)pl.getDimensionId();
                point.isRandom = true;
                point.carrier  = ((int)formGetStep(res, "carrier", {"假玩家NPC", "虚假实体(可填类型)"}, 0) >= 1) ? 1 : 0;
                {
                    std::string et = formGetString(res, "entityType", "minecraft:armor_stand");
                    point.entityType = et.empty() ? "minecraft:armor_stand" : et;
                }
                point.randomOriginX = pos.x;
                point.randomOriginZ = pos.z;
                point.randomRadius  = (int)formGetNumber(res, "radius", 1000);
                int const om = formGetStep(res, "originMode",
                                                       {"世界中心(0,0)", "创建时NPC所在位置", "触发玩家所在位置"}, 0);
                point.randomOriginMode = (om == 0) ? "world" : (om == 1 ? "npc" : "player");
                point.randomDimid   = (int)pl.getDimensionId();
                point.displayName = decodeNl(formGetString(res, "displayName"));
                if (point.displayName.empty()) point.displayName = name;
                point.message = formGetString(res, "message");
                if (point.message.empty()) point.message = "§a[随机传送] §f已传送！";
                point.floatingTextEnabled = formGetBool(res, "floatingTextEnabled", true);
                point.floatingText = decodeNl(formGetString(res, "floatingText"));
                point.floatingOffsetY = (float)formGetNumber(res, "textY", 2.0);
                if (point.floatingText.empty()) point.floatingText = "点击NPC随机传送";
                int const skinIdx = formGetStep(res, "skin", skinIds, 0);
                point.skinId   = (skinIdx >= 0 && skinIdx < (int)skinIds.size()) ? skinIds[skinIdx]
                                                                                : std::string(skins::kDefaultId);
                point.scale = 1.0f;

                if (DataStore::getInstance().addBlockTpPoint(point)) {
                    NpcTeleport::getInstance().refreshPoint(point.name);
                    tell(pl, "§a已创建随机NPC传送点 §e" + name);
                } else {
                    tell(pl, "§c创建失败（名称重复）");
                }
                openBlockTpMenu(pl);
            });
        });

    // 已有传送点
    for (auto& point : points) {
        std::string typeText = point.isRandom ? "§b随机" : "§6固定";
        std::string carrierText = (point.carrier == 1) ? "§d实体" : "§fNPC";
        std::string pname       = point.name;
        fm.appendButton("§e" + point.name + "\n§7" + typeText + " §7| " + carrierText + " §7| " +
                        dimName(point.trigger.dimid) + (point.enabled ? " §a[启用]" : " §c[禁用]"),
            tex::SETTINGS, "path", [&player, pname](Player&) { openBlockTpDetail(player, pname); });
    }

    fm.appendButton("§b重载皮肤\n§7重新扫描 npc_skins 目录（新增皮肤后点这里）", tex::INFO, "path",
        [](Player& p) {
            int const n = skins::refresh();
            tell(p, "§a已重新扫描皮肤, 当前可用 §e" + std::to_string(n) + "§a 个（含其他插件注册的）");
            openBlockTpMenu(p);
        });

    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openAdminSettingsMenu(player); });

    fm.sendTo(player);
}

// 单个 NPC 传送点的编辑: 按用途分组（基础 / 传送 / 交互 / 外观）, 每组改完回本页方便连续调。

// 从数据层取单个点位（取不到返回 false）
static bool loadPoint(std::string const& pointName, BlockTpPoint& out) {
    for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
        if (p.name == pointName) { out = p; return true; }
    }
    return false;
}

void openBlockTpDetail(Player& player, std::string const& pointName);

// 基础: 显示名 / 传送消息
static void openBtpBasic(Player& player, std::string const& pointName) {
    BlockTpPoint p;
    if (!loadPoint(pointName, p)) return openBlockTpMenu(player);

    ll::form::CustomForm fm("§l§e基础设置");
    fm.appendInput("displayName", "§e显示名称（支持 \\n 换行）", encodeNl(p.displayName), encodeNl(p.displayName));
    fm.appendInput("message", "§e传送消息", p.message, p.message);
    fm.sendTo(player, [pointName](Player& pl, ll::form::CustomFormResult const& res,
                                  ll::form::FormCancelReason) {
        if (!res.has_value()) return openBlockTpDetail(pl, pointName);
        std::string name = decodeNl(formGetString(res, "displayName"));
        std::string msg  = formGetString(res, "message");
        DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) {
            if (!name.empty()) b.displayName = name;
            if (!msg.empty()) b.message = msg;
        });
        NpcTeleport::getInstance().refreshPoint(pointName);   // 名字牌要重新下发
        tell(pl, "§a已保存基础设置");
        openBlockTpDetail(pl, pointName);
    });
}

// 传送(定点): 目标坐标 / 选公共传送点 / 用当前位置
static void openBtpTarget(Player& player, std::string const& pointName) {
    BlockTpPoint p;
    if (!loadPoint(pointName, p)) return openBlockTpMenu(player);

    std::vector<std::string> pubNames{"§7不用公共点（按下面坐标）"};
    for (auto& w : DataStore::getInstance().getPublicWarps()) pubNames.push_back(w.name);

    ll::form::CustomForm fm("§l§e定点目标");
    fm.appendDropdown("pubWarp", "§e选择公共传送点（优先于下面坐标）", pubNames, 0);
    fm.appendInput("tx", "§e目标 X", std::to_string((int)p.target.x), std::to_string((int)p.target.x));
    fm.appendInput("ty", "§e目标 Y", std::to_string((int)p.target.y), std::to_string((int)p.target.y));
    fm.appendInput("tz", "§e目标 Z", std::to_string((int)p.target.z), std::to_string((int)p.target.z));
    fm.appendStepSlider("tdim", "§e目标维度", {"主世界", "下界", "末地"},
                        (size_t)std::clamp(p.target.dimid, 0, 2));
    fm.sendTo(player, [pointName, pubNames](Player& pl, ll::form::CustomFormResult const& res,
                                  ll::form::FormCancelReason) {
        if (!res.has_value()) return openBlockTpDetail(pl, pointName);
        WarpPos np;
        int     pubIdx = formGetStep(res, "pubWarp", pubNames, 0);
        if (pubIdx > 0) {
            auto const& pubs = DataStore::getInstance().getPublicWarps();
            if (pubIdx - 1 < (int)pubs.size()) np = pubs[pubIdx - 1].pos;
        } else {
            np.x     = formGetNumber(res, "tx", 0);
            np.y     = formGetNumber(res, "ty", 64);
            np.z     = formGetNumber(res, "tz", 0);
            np.dimid = formGetStep(res, "tdim", {"主世界", "下界", "末地"}, 0);
        }
        DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) { b.target = np; });
        tell(pl, "§a已保存目标");
        openBlockTpDetail(pl, pointName);
    });
}

// 传送(随机): 半径 / 随机中心
static void openBtpRandom(Player& player, std::string const& pointName) {
    BlockTpPoint p;
    if (!loadPoint(pointName, p)) return openBlockTpMenu(player);

    ll::form::CustomForm fm("§l§e随机传送设置");
    fm.appendInput("radius", "§e随机半径（格）", std::to_string(p.randomRadius),
                   std::to_string(p.randomRadius));
    fm.appendStepSlider("originMode", "§e随机中心", {"世界中心(0,0)", "创建时NPC所在位置", "触发玩家所在位置"},
                        (size_t)(p.randomOriginMode == "world" ? 0 : (p.randomOriginMode == "player" ? 2 : 1)));
    fm.sendTo(player, [pointName](Player& pl, ll::form::CustomFormResult const& res,
                                  ll::form::FormCancelReason) {
        if (!res.has_value()) return openBlockTpDetail(pl, pointName);
        int const radius = std::max(16, (int)formGetNumber(res, "radius", 1000));
        int const om     = formGetStep(res, "originMode",
                                       {"世界中心(0,0)", "创建时NPC所在位置", "触发玩家所在位置"}, 1);
        std::string const mode = (om == 0) ? "world" : (om == 1 ? "npc" : "player");
        DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) {
            b.randomRadius     = radius;
            b.randomOriginMode = mode;
        });
        std::string const modeName = (mode == "world")    ? "世界中心(0,0)"
                                     : (mode == "player") ? "触发玩家所在位置"
                                                          : "创建时NPC所在位置";
        tell(pl, "§a已保存随机传送设置（半径 §e" + std::to_string(radius) + "§a, 中心: §e" + modeName + "§a）");
        openBlockTpDetail(pl, pointName);
    });
}

// 交互: 冷却 / 右键 / 左键 / 看向玩家
static void openBtpInteract(Player& player, std::string const& pointName) {
    BlockTpPoint p;
    if (!loadPoint(pointName, p)) return openBlockTpMenu(player);

    ll::form::CustomForm fm("§l§e交互设置");
    fm.appendInput("cd", "§e传送冷却（秒, 0 = 不限制）", std::to_string(p.cooldownSeconds),
                   std::to_string(p.cooldownSeconds));
    fm.appendToggle("onInteract", "§e右键交互触发传送", p.triggerOnInteract);
    fm.appendToggle("onAttack", "§e左键攻击触发传送", p.triggerOnAttack);
    fm.appendToggle("lookAt", "§e看向玩家（每个玩家都看到它看着自己）", p.lookAtPlayers);
    fm.sendTo(player, [pointName](Player& pl, ll::form::CustomFormResult const& res,
                                  ll::form::FormCancelReason) {
        if (!res.has_value()) return openBlockTpDetail(pl, pointName);
        int  cd         = std::max(0, (int)formGetNumber(res, "cd", 0));
        bool onInteract = formGetBool(res, "onInteract", true);
        bool onAttack   = formGetBool(res, "onAttack", true);
        bool lookAt     = formGetBool(res, "lookAt", true);
        DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) {
            b.cooldownSeconds   = cd;
            b.triggerOnInteract = onInteract;
            b.triggerOnAttack   = onAttack;
            b.lookAtPlayers     = lookAt;
        });
        NpcTeleport::getInstance().applyLookMode(pointName);   // 关掉看向玩家时立刻清除朝向覆盖
        tell(pl, "§a已保存交互设置");
        openBlockTpDetail(pl, pointName);
    });
}

// 外观: 皮肤（仅假玩家NPC）/ 悬浮字内容 / 高度
static void openBtpLook(Player& player, std::string const& pointName) {
    BlockTpPoint p;
    if (!loadPoint(pointName, p)) return openBlockTpMenu(player);

    std::vector<std::string> skinIds = skins::available();
    if (skinIds.empty()) skinIds.push_back(skins::kDefaultId);
    size_t curSkin = 0;
    for (size_t i = 0; i < skinIds.size(); ++i) {
        if (skinIds[i] == p.skinId) { curSkin = i; break; }
    }

    ll::form::CustomForm fm("§l§e外观设置");
    fm.appendDropdown("skin", "§e皮肤（仅假玩家NPC; 名单来自 npc_skins 目录）", skinIds, curSkin);
    fm.appendSlider("npcOff", "§e高度微调 · 假玩家NPC（格, 负数向下）", -3, 3, 0.25, p.npcHeightOffset);
    fm.appendSlider("entOff", "§e高度微调 · 虚假实体（格, 负数向下）", -3, 3, 0.25, p.entityHeightOffset);
    fm.appendSlider("posX", "§e位置微调 X（格, 东西向）", -8, 8, 0.25, p.posOffsetX);
    fm.appendSlider("posZ", "§e位置微调 Z（格, 南北向; 上下请用上面的高度微调）", -8, 8, 0.25, p.posOffsetZ);
    fm.appendInput("text", "§e悬浮字内容（留空 = 不显示; 支持 \\n 换行）", encodeNl(p.floatingText),
                   encodeNl(p.floatingText));
    fm.appendInput("textY", "§e悬浮字高度（格, 相对载体）", std::to_string((int)p.floatingOffsetY),
                   std::to_string((int)p.floatingOffsetY));
    fm.appendLabel("§8载体与悬浮字一起平移; 悬浮字高度是相对载体的, 改完会先收掉旧字再重建");
    fm.sendTo(player, [pointName, skinIds](Player& pl, ll::form::CustomFormResult const& res,
                                  ll::form::FormCancelReason) {
        if (!res.has_value()) return openBlockTpDetail(pl, pointName);
        std::string text = decodeNl(formGetString(res, "text"));
        float const h      = (float)formGetNumber(res, "textY", 2);
        float const npcOff = (float)formGetNumber(res, "npcOff", 0.0);
        float const entOff = (float)formGetNumber(res, "entOff", 0.0);
        float const px     = (float)formGetNumber(res, "posX", 0.0);
        float const pz     = (float)formGetNumber(res, "posZ", 0.0);
        int const   skinIdx = formGetStep(res, "skin", skinIds, 0);
        std::string const newSkin =
            (skinIdx >= 0 && skinIdx < (int)skinIds.size()) ? skinIds[skinIdx] : std::string(skins::kDefaultId);
        DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) {
            b.skinId              = newSkin;
            b.npcHeightOffset     = npcOff;
            b.entityHeightOffset  = entOff;
            b.posOffsetX          = px;
            b.posOffsetZ          = pz;
            b.floatingText        = text;
            b.floatingTextEnabled = !text.empty();
            b.floatingOffsetY     = h;
        });
        NpcTeleport::getInstance().refreshPoint(pointName);
        char bufN[16], bufE[16], bufP[48];
        std::snprintf(bufN, sizeof bufN, "%+.2f", npcOff);
        std::snprintf(bufE, sizeof bufE, "%+.2f", entOff);
        std::snprintf(bufP, sizeof bufP, "%+.2f/%+.2f", px, pz);
        tell(pl, std::string("§a已保存外观设置（皮肤: ") + newSkin + ", 高度微调 NPC " + bufN +
                     " / 实体 " + bufE + ", 位置微调 " + bufP + "）");
        openBlockTpDetail(pl, pointName);
    });
}

// 点位主页: 概况 + 分组入口 + 载体/开关/删除
void openBlockTpDetail(Player& player, std::string const& pointName) {
    BlockTpPoint point;
    if (!loadPoint(pointName, point)) {
        tell(player, "§c该NPC传送点已不存在");
        return openBlockTpMenu(player);
    }

    std::string const modeText = (point.randomOriginMode == "world")    ? "世界中心(0,0)"
                                 : (point.randomOriginMode == "player") ? "触发玩家所在位置"
                                                                        : "创建时NPC所在位置";
    char offsetBuf[40];
    std::snprintf(offsetBuf, sizeof offsetBuf, "%+.2f", point.entityHeightOffset);
    char npcOffsetBuf[40];
    std::snprintf(npcOffsetBuf, sizeof npcOffsetBuf, "%+.2f", point.npcHeightOffset);
    char posOffsetBuf[48];
    std::snprintf(posOffsetBuf, sizeof posOffsetBuf, "%+.2f/%+.2f", point.posOffsetX, point.posOffsetZ);
    bool const hasPosOffset = point.posOffsetX != 0.0f || point.posOffsetZ != 0.0f;

    std::string const carrierText =
        (point.carrier == 1
             ? ("虚假实体 §8(" + point.entityType + "§8) §7高度 §f" + offsetBuf)
             : ("假玩家NPC §8皮肤: " + (point.skinId.empty() ? std::string("默认") : point.skinId) +
                "§7 高度 §f" + npcOffsetBuf)) +
        (hasPosOffset ? ("§7 位置 §f" + std::string(posOffsetBuf)) : std::string{});

    ll::form::SimpleForm fm(
        "§l§e" + point.name,
        "§r载体: §f" + carrierText +
            "\n§7类型: " + (point.isRandom ? "§b随机传送" : "§6固定传送") +
            "\n§7位置: §f" + dimName(point.trigger.dimid) + " §8(" +
            std::to_string((int)point.trigger.x) + ", " + std::to_string((int)point.trigger.y) + ", " +
            std::to_string((int)point.trigger.z) + ")" +
            (point.isRandom
                 ? (std::string("\n§7随机: 半径 §e") + std::to_string(point.randomRadius) + "§7, 中心 §f" + modeText)
                 : (std::string("\n§7目标: §f") + dimName(point.target.dimid) + " §8(" +
                    std::to_string((int)point.target.x) + ", " + std::to_string((int)point.target.y) +
                    ", " + std::to_string((int)point.target.z) + ")")) +
            "\n§7冷却: §f" + (point.cooldownSeconds > 0 ? (std::to_string(point.cooldownSeconds) + " 秒")
                                                       : "不限制") +
            "\n§7状态: " + (point.enabled ? "§a启用" : "§c禁用"));

    fm.appendButton("§e基础设置\n§7显示名称、传送消息", tex::INFO, "path",
                    [pointName](Player& p) { openBtpBasic(p, pointName); });
    fm.appendButton(point.isRandom ? "§b随机传送设置\n§7半径、随机中心" : "§b定点目标\n§7坐标 / 公共传送点 / 当前位置",
                    tex::WARP, "path", [pointName, isRandom = point.isRandom](Player& p) {
                        if (isRandom) openBtpRandom(p, pointName);
                        else openBtpTarget(p, pointName);
                    });
    fm.appendButton("§d交互设置\n§7冷却、右键/左键触发、看向玩家", tex::PLAYER, "path",
                    [pointName](Player& p) { openBtpInteract(p, pointName); });
    fm.appendButton("§6外观设置\n§7皮肤、高度微调、悬浮字", tex::SETTINGS, "path",
                    [pointName](Player& p) { openBtpLook(p, pointName); });

    fm.appendButton("§e切换载体类型\n§7假玩家NPC ↔ 虚假实体", tex::PLAYER, "path",
                    [pointName](Player& p) {
                        bool nowEntity = false;
                        bool ok        = DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) {
                            b.carrier = (b.carrier == 1) ? 0 : 1;
                            nowEntity = (b.carrier == 1);
                        });
                        if (ok) {
                            NpcTeleport::getInstance().refreshPoint(pointName);
                            tell(p, nowEntity ? "§a已切换为 §e虚假实体§a 载体"
                                              : "§a已切换为 §e假玩家NPC§a 载体");
                        } else {
                            tell(p, "§c切换失败");
                        }
                        openBlockTpDetail(p, pointName);
                    });
    // 定点 ↔ 随机: 转换时把对侧缺的参数补成"载体位置"这类合理默认, 免得出现半径 0 / 目标 (0,0,0)
    fm.appendButton(point.isRandom ? "§e切换为定点传送\n§7当前: §b随机传送" : "§e切换为随机传送\n§7当前: §6定点传送",
                    tex::WARP, "path", [pointName](Player& p) {
                        bool nowRandom = false;
                        bool ok        = DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) {
                            b.isRandom = !b.isRandom;
                            nowRandom  = b.isRandom;
                            if (b.isRandom) {
                                if (b.randomRadius <= 0) b.randomRadius = 1000;
                                if (b.randomOriginMode.empty()) b.randomOriginMode = "npc";
                                b.randomOriginX = b.trigger.x;
                                b.randomOriginZ = b.trigger.z;
                                b.randomDimid   = b.trigger.dimid;
                            } else if (b.target.x == 0.0 && b.target.y == 0.0 && b.target.z == 0.0) {
                                b.target.x     = b.trigger.x;
                                b.target.y     = b.trigger.y;
                                b.target.z     = b.trigger.z;
                                b.target.dimid = b.trigger.dimid;
                            }
                        });
                        if (ok) {
                            NpcTeleport::getInstance().refreshPoint(pointName);
                            tell(p, nowRandom
                                       ? "§a已切换为 §b随机传送§a（中心默认载体位置、半径 1000, 可在「随机传送设置」改）"
                                       : "§a已切换为 §6定点传送§a（目标默认载体位置, 可在「定点目标」改）");
                        } else {
                            tell(p, "§c切换失败");
                        }
                        openBlockTpDetail(p, pointName);
                    });
    fm.appendButton(point.enabled ? "§c禁用此传送点" : "§a启用此传送点", tex::CANCEL, "path",
                    [pointName](Player& p) {
                        bool nowEnabled = false;
                        DataStore::getInstance().mutateBlockTpPoint(pointName, [&](BlockTpPoint& b) {
                            b.enabled  = !b.enabled;
                            nowEnabled = b.enabled;
                        });
                        NpcTeleport::getInstance().refreshPoint(pointName);
                        tell(p, nowEnabled ? "§a已启用" : "§c已禁用");
                        openBlockTpDetail(p, pointName);
                    });
    fm.appendButton("§c删除此传送点\n§7不可恢复", tex::DELETE, "path", [pointName](Player& p) {
        if (DataStore::getInstance().removeBlockTpPoint(pointName)) {
            NpcTeleport::getInstance().refreshPoint(pointName);
            tell(p, "§c已删除NPC传送点 §e" + pointName);
        } else {
            tell(p, "§c删除失败");
        }
        openBlockTpMenu(p);
    });
    fm.appendButton("§7返回列表", tex::BACK, "path", [](Player& p) { openBlockTpMenu(p); });

    fm.sendTo(player);
}

// 系统管理（管理员）
} // namespace menu
} // namespace mtps
