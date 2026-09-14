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

void openTpaSelectForm(Player& player) {
    // 目标列表: 第一项是"没选"占位, 第二项是召集, 之后才是在线玩家
    std::vector<std::string> options{"§7请选择…"};
    options.push_back(std::string("§6召集传送 §7(把所有人叫到我身边)") +
                      (Config::getInstance().rallyEnabled() ? "" : " §8[未开启]"));
    auto                     level = ll::service::getLevel();
    if (level) {
        level->forEachPlayer([&](Player& p) -> bool {
            if (p.getRealName() == player.getRealName()) return true;
            // 黑名单: 把我拉黑的人不出现在我的目标列表里
            auto blk = DataStore::getInstance().getBlockedPlayers(p.getRealName());
            if (std::find(blk.begin(), blk.end(), player.getRealName()) != blk.end()) return true;
            options.push_back(p.getRealName());
            return true;
        });
    }

    ll::form::CustomForm fm("§l§b发起传送 / 召集");
    fm.appendDropdown("target", "§e选择目标", options, 0);
    fm.appendStepSlider("type", "§e传送类型（选召集时忽略）", {"我传送到对方", "对方传送到我"}, 0);
    fm.appendSlider("duration", "§e请求有效时长（秒）", 30, 120, 1, 60);

    fm.sendTo(player, [options](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) return;
        // 下拉框回传的可能是下标, 也可能是被选中的文本, 用 formGetStep 两种都认
        int const idx = formGetStep(res, "target", options, 0);
        if (idx <= 0) {
            tell(pl, "§c请先选择一个目标");
            return openTpaSelectForm(pl);
        }
        if (idx == 1) {                       // 召集: 不看传送类型, 统一"所有人传送到我"
            TpaRally::getInstance().startRally(pl);
            return;
        }
        // options 是"占位 + 召集 + 玩家", 所以选中的玩家名就是 options[idx] 本身
        // （原来写成 options[idx - 2], 于是永远取到占位/召集那两项, 报"玩家不在线"）
        if (idx >= (int)options.size()) return;
        std::string const targetName = options[idx];

        int const   typeIdx  = formGetStep(res, "type", {"我传送到对方", "对方传送到我"}, 0);
        std::string tpaType  = (typeIdx == 0) ? "tpa" : "tpahere";
        int const   duration = (int)formGetNumber(res, "duration", 60);
        TpaRally::getInstance().sendTpaRequest(pl, targetName, tpaType, duration);
    });
}

// 请求管理
void openRequestManageForm(Player& player) {
    auto requests = TpaRally::getInstance().getRequestsFor(player.getRealName());

    ll::form::SimpleForm fm("§l§6请求管理",
        requests.empty() ? "§r暂无待处理的请求" : "§r共有 §e" + std::to_string(requests.size()) + "§r 条请求");

    if (requests.empty()) {
        fm.appendButton("§7返回主菜单", tex::BACK, "path",
            [&player](Player&) { openMainMenu(player); });
        fm.sendTo(player);
        return;
    }

    for (auto& req : requests) {
        // 召集: 发起者自己那条点了是取消, 其他人那条点了是加入
        if (req.type == "rally") {
            if (req.fromName == player.getRealName()) {
                fm.appendButton("§6召集传送 §7(你发起的)\n§7已响应 §e" + std::to_string(req.joiners) +
                                    "§7 人 §8(点击取消召集)",
                                tex::CANCEL, "path",
                                [&player](Player&) { TpaRally::getInstance().cancelRally(player); });
            } else {
                fm.appendButton("§6召集传送\n§7§e" + req.fromName + "§7 邀请你传送到TA身边 §8(点击加入)",
                                tex::ACCEPT, "path",
                                [&player](Player&) { TpaRally::getInstance().joinRally(player); });
            }
            continue;
        }
        bool isIncoming = (req.toName == player.getRealName());
        if (isIncoming) {
            std::string const from     = req.fromName;
            std::string const typeText = (req.type == "tpahere") ? "想让你传送到TA身边" : "想传送到你身边";
            fm.appendButton("§e" + from + "\n§7" + typeText + " §8(点击同意)", tex::ACCEPT, "path",
                [&player, from](Player&) { TpaRally::getInstance().acceptRequest(player, from); });
        } else {
            std::string const to       = req.toName;
            std::string const typeText = (req.type == "tpahere") ? "对方传送到我" : "我传送到对方";
            fm.appendButton("§e我 → " + to + "\n§7" + typeText + " §8(点击取消)", tex::CANCEL, "path",
                [&player, to](Player&) { TpaRally::getInstance().cancelRequest(player, to); });
        }
    }
    fm.appendButton("§7返回主菜单", tex::BACK, "path",
        [&player](Player&) { openMainMenu(player); });
    fm.sendTo(player);
}

// 个人设置
void startRally(Player& player) { TpaRally::getInstance().startRally(player); }
void acceptTeleportRequest(Player& player) { TpaRally::getInstance().acceptRequest(player); }
void refuseTeleportRequest(Player& player) { TpaRally::getInstance().refuseRequest(player); }

} // namespace menu
} // namespace mtps
