#include "Menu.h"
#include "Config.h"
#include "DataStore.h"
#include "Economy.h"
#include "RandomTeleport.h"
#include "WarpManager.h"
#include "TpaRally.h"
#include "NpcTeleport.h"
#include "DataTypes.h"
#include <mc/network/MinecraftPacketIds.h>
#include <mc/network/MinecraftPackets.h>
#include <mc/network/packet/TransferPacket.h>
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

static std::unordered_map<std::string, std::string> onlineNamesByXuid() {
    std::unordered_map<std::string, std::string> m;
    auto level = ll::service::getLevel();
    if (!level) return m;
    for (auto& [uuid, entry] : level->getPlayerList()) {
        std::string xuid = entry.mXUID;
        std::string name = entry.mName;
        if (!xuid.empty() && !name.empty()) m[xuid] = name;
    }
    // 兜底: 名单为空（或个别玩家还没进名单）时用在线玩家补齐
    level->forEachPlayer([&](Player& p) -> bool {
        if (!p.getXuid().empty()) m[p.getXuid()] = p.getRealName();
        return true;
    });
    return m;
}
static std::string nameOfXuid(std::unordered_map<std::string, std::string> const& m, std::string const& xuid) {
    auto it = m.find(xuid);
    return it != m.end() ? it->second : xuid;
}

// 跨服传送: 列出目的地, 确认后让客户端转服（TransferPacket）
// 数据在 Meowdata/Mtps/CrossServer.json

void openCrossServerList(Player& player);
static void openCrossServerManage(Player& player);
static void openCrossServerEdit(Player& player, std::string const& name);

void openCrossServerList(Player& player) {
    std::vector<CrossServerEntry> usable;
    for (auto& c : DataStore::getInstance().getCrossServers()) {
        if (c.enabled) usable.push_back(c);
    }

    ll::form::SimpleForm fm("§l§d跨服传送",
                            "§r共 §e" + std::to_string(usable.size()) + "§r 个目的地\n"
                            "§7确认后客户端会连接到对应服务器");

    for (auto& e : usable) {
        std::string const label = e.name + "\n§7" + e.address + ":" + std::to_string(e.port);
        fm.appendButton(label, tex::PUBLIC, "path", [e](Player& p) {
            ll::form::SimpleForm cf("§l§6跨服传送",
                                    "§r即将传送到 §e" + e.name + "\n§7地址: §f" + e.address + ":" +
                                        std::to_string(e.port) + "\n\n§c确认后客户端会断开当前服务器并连接过去");
            cf.appendButton("§a确认传送", tex::ACCEPT, "path", [e](Player& p2) {
                // 让引擎自己造包再填字段: 直接按值构造 TransferPacket 会引用到 payload 里
                // GatheringsConfigurationJoinInfo 的析构, 而它在服务端目标里没有导出(MCNAPI),
                // 链接会失败。走 createPacket 则由引擎内部构造/析构, 插件侧不需要那个符号。
                auto pkt = MinecraftPackets::createPacket(MinecraftPacketIds::Transfer);
                auto* tp = static_cast<TransferPacket*>(pkt.get());
                if (pkt && tp) {
                    tp->mDestination     = e.address;
                    tp->mDestinationPort = (ushort)e.port;
                    tp->mReloadWorld     = false;
                    p2.sendNetworkPacket(*pkt);
                    tell(p2, "§a[跨服] §f正在传送到 §e" + e.name + "§f …");
                } else {
                    tell(p2, "§c[跨服] §f传送包构造失败");
                }
            });
            cf.appendButton("§7取消", tex::CANCEL, "path", [](Player& p2) { openCrossServerList(p2); });
            cf.sendTo(p);
        });
    }
    if (isOp(player)) {
        fm.appendButton("§4[管理] 跨服目的地\n§7增删改", tex::SETTINGS, "path",
                        [](Player& p) { openCrossServerManage(p); });
    }
    fm.appendButton("§7返回", tex::BACK, "path", [](Player& p) { openWarpMenu(p); });
    fm.sendTo(player);
}

static void openCrossServerManage(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能管理跨服目的地！");
        return openWarpMenu(player);
    }
    auto const& list = DataStore::getInstance().getCrossServers();

    ll::form::SimpleForm fm("§l§4跨服目的地管理",
                            "§r共 §e" + std::to_string(list.size()) + "§r 个\n§7点击可编辑, 或新建一个");
    fm.appendButton("§a新建目的地\n§7填名称/地址/端口", tex::ADD, "path",
                    [](Player& p) { openCrossServerEdit(p, ""); });
    for (auto& e : list) {
        std::string name = e.name;
        fm.appendButton((e.enabled ? "§e" : "§8[禁用] ") + e.name + "\n§7" + e.address + ":" +
                            std::to_string(e.port),
                        tex::SETTINGS, "path", [name](Player& p) { openCrossServerEdit(p, name); });
    }
    fm.appendButton("§7返回", tex::BACK, "path", [](Player& p) { openCrossServerList(p); });
    fm.sendTo(player);
}

static void openCrossServerEdit(Player& player, std::string const& name) {
    if (!isOp(player)) return openWarpMenu(player);

    CrossServerEntry entry;
    bool             isNew = name.empty();
    if (!isNew) {
        bool found = false;
        for (auto& e : DataStore::getInstance().getCrossServers()) {
            if (e.name == name) { entry = e; found = true; break; }
        }
        if (!found) return openCrossServerManage(player);
    } else {
        entry.port = 19132;
    }

    ll::form::CustomForm fm(isNew ? "§l§a新建跨服目的地" : "§l§e编辑: " + entry.name);
    fm.appendInput("name", "§e显示名称", entry.name, entry.name);
    fm.appendInput("address", "§e服务器地址（域名或 IP）", entry.address, entry.address);
    fm.appendInput("port", "§e端口", std::to_string(entry.port), std::to_string(entry.port));
    fm.appendToggle("enabled", "§e启用（玩家可见）", entry.enabled);
    fm.sendTo(player, [name, isNew](Player& pl, ll::form::CustomFormResult const& res,
                                    ll::form::FormCancelReason) {
        if (!res.has_value()) return openCrossServerManage(pl);
        CrossServerEntry e;
        e.name    = formGetString(res, "name");
        e.address = formGetString(res, "address");
        e.port    = (int)formGetNumber(res, "port", 19132);
        e.enabled = formGetBool(res, "enabled", true);
        if (e.name.empty() || e.address.empty()) {
            tell(pl, "§c名称和地址都不能为空");
            return openCrossServerManage(pl);
        }

        auto& ds = DataStore::getInstance();
        bool  ok;
        if (isNew) {
            ok = ds.addCrossServer(e);
        } else if (e.name == name) {
            ok = ds.updateCrossServer(e);
        } else {   // 改了名字: 先删旧的再加新的
            ds.removeCrossServer(name);
            ok = ds.addCrossServer(e);
        }
        tell(pl, ok ? ("§a已保存 §e" + e.name) : "§c保存失败（名称重复?）");
        openCrossServerManage(pl);
    });
}

void openWarpMenu(Player& player) {
    auto& cfg = Config::getInstance();
    auto& ds = DataStore::getInstance();

    auto privateWarps = ds.getPrivateWarps(player.getXuid());
    int maxWarps = cfg.maxPrivateWarps();

    std::string content;
    if (cfg.isFeatureEnabled("privateWarps")) {
        content += "§r私人传送点: §e" + std::to_string(privateWarps.size()) + "§r / §a" + std::to_string(maxWarps) + "§r 个\n";
    }
    if (cfg.isFeatureEnabled("publicWarps")) {
        content += "§r公共传送点: §e" + std::to_string(ds.getPublicWarps().size()) + "§r 个\n";
    }
    if (cfg.isFeatureEnabled("noApprovalWarps")) {
        content += "§r免申请传送点: §e" + std::to_string(ds.countNoApprovalWarps()) + "§r 个\n";
    }
    if (content.empty()) content = "§7暂无可用功能";

    ll::form::SimpleForm fm("§l§a传送点系统", content);

    if (cfg.isFeatureEnabled("privateWarps")) {
        fm.appendButton("§b我的传送点\n§7点击传送 | 管理传送点", tex::PRIVATE, "path",
            [&player](Player&) { openPrivateWarpMenu(player); });
    }
    if (cfg.isFeatureEnabled("publicWarps")) {
        fm.appendButton("§6公共传送点\n§7点击直接传送", tex::WARP, "path",
            [&player](Player&) { openPublicWarpQuickList(player); });
    }
    if (cfg.isFeatureEnabled("noApprovalWarps")) {
        fm.appendButton("§a免申请传送点\n§7点击直接传送 | 可搜索他人共享点", tex::TELEPORT, "path",
            [&player](Player&) { openNoApprovalWarpsList(player); });
    }
    // 管理员: 官方(公共)传送点 / 所有玩家传送点, 与玩家自己的传送点归到同一个菜单下
    if (Config::getInstance().isFeatureEnabled("crossServer")) {
        fm.appendButton("§d跨服传送\n§7转到其它服务器", tex::PUBLIC, "path",
            [](Player& p) { openCrossServerList(p); });
    }
    if (isOp(player)) {
        fm.appendButton("§4[管理] 官方传送点\n§7公共传送点增删、改坐标", tex::SETTINGS, "path",
            [&player](Player&) { openPublicWarpManageMenu(player); });
    }
    fm.appendButton("§7返回主菜单", tex::BACK, "path",
        [&player](Player&) { openMainMenu(player); });

    fm.sendTo(player);
}

// 随机传送菜单

void openPrivateWarpMenu(Player& player, std::string const& actionStr) {
    auto& cfg = Config::getInstance();
    auto& ds = DataStore::getInstance();
    auto const& warps = ds.getPrivateWarps(player.getXuid());
    int maxWarps = cfg.maxPrivateWarps();
    int cost = cfg.getCost("privateWarp");

    // 编号直达（指令名取自配置）
    if (!actionStr.empty()) {
        int num = 0;
        try { num = std::stoi(actionStr); } catch (...) { num = 0; }
        if (num >= 1 && num <= (int)warps.size()) {
            WarpManager::getInstance().teleportToPrivateWarp(player, warps[num - 1].name);
            return;
        }
        if (!warps.empty()) {
            tell(player, "§c[私人传送点] §f无效编号 §e" + actionStr + "§f，可用范围 §a1~" +
                 std::to_string(warps.size()) + "§f（§e/" + cfg.getCommand("private") + " 编号§f 直达）");
        } else {
            tell(player, "§c[私人传送点] §f暂无私人传送点");
        }
        return;
    }

    std::string costText = cost > 0 ? " §7(消耗 " + formatCost(cost) + ")" : "";
    ll::form::SimpleForm fm("§l§b私人传送点",
        "§r已创建: §e" + std::to_string(warps.size()) + "§r / §a" + std::to_string(maxWarps) +
        "§r 个传送点\n§7点击传送点 §a直接传送" + costText + "\n§7快捷指令: §e/" + cfg.getCommand("private") +
        " 编号§r 可直达对应传送点");

    // 添加传送点
    fm.appendButton("§a添加传送点\n§7保存当前位置", tex::ADD, "path", [&player, maxWarps](Player&) {
        auto& ds2 = DataStore::getInstance();
        if (ds2.getPrivateWarpCount(player.getXuid()) >= maxWarps) {
            tell(player, "§c传送点数量已达上限 (" + std::to_string(maxWarps) + "个)！");
            return openPrivateWarpMenu(player);
        }
        // 输入名称
        ll::form::CustomForm form("§l§a添加传送点");
        form.appendInput("name", "§e传送点名称", "输入名称");
        form.sendTo(player, [maxWarps](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
            if (!res.has_value()) return;
            auto it = res->find("name");
            if (it == res->end()) return;
            std::string name = std::get_if<std::string>(&it->second) ? *std::get_if<std::string>(&it->second) : "";
            if (name.empty()) { tell(pl, "§c名称不能为空"); return; }
            if (WarpManager::getInstance().addPrivateWarp(pl, name)) {
                tell(pl, "§a已添加传送点 §e" + name);
            } else {
                tell(pl, "§c添加失败（名称重复或已达上限）");
            }
        });
    });

    // 各传送点: 点击直接传送
    for (auto& warp : warps) {
        std::string tag = warp.noApproval ? "§a[免] " : "";
        fm.appendButton(tag + "§e" + warp.name + "\n§7" + dimName(warp.pos.dimid) + " | 点击传送",
            tex::TELEPORT, "path", [&player, warp](Player&) {
                WarpManager::getInstance().teleportToPrivateWarp(player, warp.name);
            });
    }

    fm.appendButton("§e管理传送点\n§7免申请、删除、重设坐标", tex::SETTINGS, "path",
        [&player](Player&) { openPrivateWarpManageMenu(player); });
    // 自定义半径（填入数值式, 不受预设限制）
    fm.appendButton("§a自定义半径传送\n§7自己填入半径数值", tex::ADD, "path",
        [&player](Player&) {
            ll::form::CustomForm form("§l§a自定义半径随机传送");
            form.appendInput("radius", "§e随机半径（格, 直接填数字）", "1000", "1000");
            form.appendToggle("usePlayer", "§e以我当前位置为中心（关闭 = 以世界原点）", true);
            form.sendTo(player, [](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
                if (!res.has_value()) return openRandomTeleportMenu(pl);
                RtpOptions opts;
                opts.dimid      = (int)pl.getDimensionId();
                opts.originMode = formGetBool(res, "usePlayer", true) ? "player" : "fixed";
                opts.originX    = 0;
                opts.originZ    = 0;
                opts.radius     = std::max(50, (int)formGetNumber(res, "radius", 1000));
                opts.message    = "§a[随机传送] §f已传送到随机位置（半径 " + std::to_string(opts.radius) + "）！";
                opts.cost       = 0;
                tell(pl, "§7[随机传送] 半径 §e" + std::to_string(opts.radius) + "§7 开始搜索…");
                RandomTeleport::getInstance().start(pl, opts);
            });
        });
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openWarpMenu(player); });

    fm.sendTo(player);
}


// 管理员: 填入坐标修改传送点（输入框默认 = 管理员当前坐标）
// 仅管理员可用（调用方需先判 isOp）; onApply 负责真正写入数据层, onBack 负责返回上级表单
static void openCoordEditForm(Player& player, std::string const& title,
                              std::function<void(Player&, WarpPos const&)> onApply,
                              std::function<void(Player&)> onBack) {
    auto pos = player.getPosition();
    int  px = (int)std::floor(pos.x);
    int  py = (int)std::floor(pos.y);
    int  pz = (int)std::floor(pos.z);
    int  pd = std::clamp((int)player.getDimensionId(), 0, 2);

    ll::form::CustomForm fm("§l§e" + title);
    fm.appendInput("x", "§eX 坐标", std::to_string(px), std::to_string(px));
    fm.appendInput("y", "§eY 坐标", std::to_string(py), std::to_string(py));
    fm.appendInput("z", "§eZ 坐标", std::to_string(pz), std::to_string(pz));
    fm.appendStepSlider("dim", "§e维度", {"主世界", "下界", "末地"}, (size_t)pd);
    fm.appendLabel("§7输入框已默认填入你当前所在的位置, 直接改成目标坐标即可");
    fm.sendTo(player, [onApply, onBack](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) {
            if (onBack) onBack(pl);
            return;
        }
        auto toInt = [&res](char const* key, int def) -> int {
            std::string v = formGetString(res, key);
            // 去首尾空白后解析; 非法输入回落默认值
            size_t b = v.find_first_not_of(" \t\r\n");
            size_t e = v.find_last_not_of(" \t\r\n");
            if (b == std::string::npos) return def;
            v = v.substr(b, e - b + 1);
            try {
                return (int)std::lround(std::stod(v));
            } catch (...) {
                return def;
            }
        };
        auto      p = pl.getPosition();
        WarpPos   np;
        np.x     = toInt("x", (int)std::floor(p.x));
        np.y     = toInt("y", (int)std::floor(p.y));
        np.z     = toInt("z", (int)std::floor(p.z));
        np.dimid = (int)formGetStep(res, "dim", {"主世界", "下界", "末地"}, 0);
        if (onApply) onApply(pl, np);
    });
}

// 管理我的传送点（免申请开关、删除、[新增]重设坐标到当前位置）
void openPrivateWarpManageMenu(Player& player) {
    auto& ds = DataStore::getInstance();
    auto const& warps = ds.getPrivateWarps(player.getXuid());

    if (warps.empty()) {
        tell(player, "§c暂无可管理的传送点！");
        return openPrivateWarpMenu(player);
    }

    ll::form::SimpleForm fm("§l§e管理传送点",
        "§r选择要管理的传送点\n§7可开启免申请、删除、§a重设坐标到当前位置§7（新）");

    for (auto& warp : warps) {
        std::string tag = warp.noApproval ? "§a[免申请] " : "§7[需申请] ";
        fm.appendButton(tag + "§e" + warp.name + "\n§7" + dimName(warp.pos.dimid) +
                        " §8(" + std::to_string((int)warp.pos.x) + ", " + std::to_string((int)warp.pos.y) +
                        ", " + std::to_string((int)warp.pos.z) + ")",
            tex::SETTINGS, "path", [&player, warp](Player&) {
                ll::form::SimpleForm fm2("§l§e" + warp.name,
                    "§r传送点: §e" + warp.name + "\n§7位置: §f" + dimName(warp.pos.dimid) +
                    " §8(" + std::to_string((int)warp.pos.x) + ", " + std::to_string((int)warp.pos.y) +
                    ", " + std::to_string((int)warp.pos.z) + ")\n§7免申请: " +
                    (warp.noApproval ? "§a开" : "§c关"));

                std::string warpName = warp.name;
                // [新增] 重设坐标到当前位置
                fm2.appendButton("§a重设坐标到当前位置\n§7将此传送点坐标更新为你的位置", tex::WARP, "path",
                    [&player, warpName](Player&) {
                        if (WarpManager::getInstance().resetPrivateWarpPosHere(player, warpName)) {
                            tell(player, "§a已重设传送点 §e" + warpName + " §a坐标到当前位置");
                        } else {
                            tell(player, "§c重设失败");
                        }
                    });
                // 管理员: 填入坐标修改（输入框默认管理员当前坐标）
                if (isOp(player)) {
                    fm2.appendButton("§e填入坐标修改 §8[管理]\n§7输入框默认你的当前坐标", tex::SETTINGS, "path",
                        [&player, warpName](Player&) {
                            std::string xuid = player.getXuid();
                            openCoordEditForm(
                                player, "填入坐标修改: " + warpName,
                                [xuid, warpName](Player& pl, WarpPos const& np) {
                                    if (DataStore::getInstance().mutatePrivateWarp(
                                            xuid, warpName, [&](PrivateWarp& w) { w.pos = np; })) {
                                        tell(pl, "§a已修改 §e" + warpName + "§a 坐标为 §e(" +
                                                 std::to_string((int)np.x) + ", " +
                                                 std::to_string((int)np.y) + ", " +
                                                 std::to_string((int)np.z) + ") §7" + dimName(np.dimid));
                                    } else {
                                        tell(pl, "§c修改失败");
                                    }
                                },
                                [&player](Player& pl) { openPrivateWarpManageMenu(pl); });
                        });
                }
                // 免申请开关
                std::string toggleText = warp.noApproval ? "§c关闭免申请" : "§a开启免申请";
                fm2.appendButton(toggleText + "\n§7其他玩家无需申请即可传送到此点", tex::ACCEPT, "path",
                    [&player, warpName](Player&) {
                        bool on = false;
                        if (WarpManager::getInstance().toggleNoApproval(player, warpName, on)) {
                            tell(player, on ? "§a已开启免申请" : "§c已关闭免申请");
                        } else {
                            tell(player, "§c操作失败");
                        }
                    });
                // 删除
                fm2.appendButton("§c删除此传送点\n§7不可恢复", tex::DELETE, "path",
                    [&player, warpName](Player&) {
                        if (WarpManager::getInstance().removePrivateWarp(player, warpName)) {
                            tell(player, "§c已删除传送点 §e" + warpName);
                        } else {
                            tell(player, "§c删除失败");
                        }
                    });
                fm2.appendButton("§7返回", tex::BACK, "path",
                    [&player](Player&) { openPrivateWarpManageMenu(player); });

                fm2.sendTo(player);
            });
    }
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openPrivateWarpMenu(player); });

    fm.sendTo(player);
}

// 公共传送点列表
void openPublicWarpQuickList(Player& player, std::string const& actionStr) {
    auto& ds = DataStore::getInstance();
    auto const& warps = ds.getPublicWarps();

    if (warps.empty()) {
        tell(player, "§c暂无公共传送点！");
        return openWarpMenu(player);
    }

    // 编号直达（指令名取自配置）
    if (!actionStr.empty()) {
        int num = 0;
        try { num = std::stoi(actionStr); } catch (...) { num = 0; }
        if (num >= 1 && num <= (int)warps.size()) {
            WarpManager::getInstance().teleportToPublicWarp(player, warps[num - 1].name);
            return;
        }
        tell(player, "§c[公共传送点] §f无效编号 §e" + actionStr + "§f，可用范围 §a1~" +
             std::to_string(warps.size()) + "§f（§e/" + Config::getInstance().getCommand("public") + " 编号§f 直达）");
        return;
    }

    ll::form::SimpleForm fm("§l§6公共传送点",
        "§r选择目的地直接传送\n§7快捷指令: §e/" + Config::getInstance().getCommand("public") +
        " 编号§r 可直达对应传送点");

    for (auto& warp : warps) {
        fm.appendButton("§e" + warp.name + "\n§7" + dimName(warp.pos.dimid), tex::TELEPORT, "path",
            [&player, warp](Player&) {
                WarpManager::getInstance().teleportToPublicWarp(player, warp.name);
            });
    }
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openWarpMenu(player); });

    fm.sendTo(player);
}

// 管理公共传送点（管理员: 添加、删除、[新增]修改坐标到当前位置）
void openPublicWarpManageMenu(Player& player) {
    if (!isOp(player)) {
        tell(player, "§c只有管理员才能管理公共传送点！");
        return openWarpMenu(player);
    }

    auto& ds = DataStore::getInstance();
    auto const& warps = ds.getPublicWarps();

    ll::form::SimpleForm fm("§l§4管理公共传送点",
        "§r共有 §e" + std::to_string(warps.size()) + "§r 个公共传送点");

    fm.appendButton("§a添加传送点\n§7在当前位置创建", tex::ADD, "path", [&player](Player&) {
        ll::form::CustomForm form("§l§a添加公共传送点");
        form.appendInput("name", "§e传送点名称", "输入名称");
        form.sendTo(player, [](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
            if (!res.has_value()) return;
            auto it = res->find("name");
            if (it == res->end()) return;
            std::string name = std::get_if<std::string>(&it->second) ? *std::get_if<std::string>(&it->second) : "";
            if (name.empty()) { tell(pl, "§c名称不能为空"); return; }
            if (WarpManager::getInstance().addPublicWarp(pl, name)) {
                tell(pl, "§a已添加公共传送点 §e" + name);
            } else {
                tell(pl, "§c添加失败（名称重复）");
            }
        });
    });

    for (auto& warp : warps) {
        fm.appendButton("§e" + warp.name + "\n§7" + dimName(warp.pos.dimid) +
                        " §8(" + std::to_string((int)warp.pos.x) + ", " + std::to_string((int)warp.pos.y) +
                        ", " + std::to_string((int)warp.pos.z) + ")",
            tex::SETTINGS, "path", [&player, warp](Player&) {
                ll::form::SimpleForm fm2("§l§e" + warp.name,
                    "§r公共传送点: §e" + warp.name + "\n§7创建者: §f" + warp.owner +
                    "\n§7位置: §f" + dimName(warp.pos.dimid) + " §8(" +
                    std::to_string((int)warp.pos.x) + ", " + std::to_string((int)warp.pos.y) +
                    ", " + std::to_string((int)warp.pos.z) + ")");

                std::string warpName = warp.name;
                // [新增] 管理员修改坐标到当前位置
                fm2.appendButton("§a修改坐标到当前位置\n§7将此公共传送点坐标更新为你的位置", tex::WARP, "path",
                    [&player, warpName](Player&) {
                        if (WarpManager::getInstance().modifyPublicWarpPosHere(player, warpName)) {
                            tell(player, "§a已修改公共传送点 §e" + warpName + " §a坐标到当前位置");
                        } else {
                            tell(player, "§c修改失败");
                        }
                    });
                // 填入坐标修改（输入框默认管理员当前坐标）
                fm2.appendButton("§e填入坐标修改\n§7输入框默认你的当前坐标", tex::SETTINGS, "path",
                    [&player, warpName](Player&) {
                        std::string wn = warpName;
                        openCoordEditForm(
                            player, "填入坐标修改: " + wn,
                            [wn](Player& pl, WarpPos const& np) {
                                if (DataStore::getInstance().updatePublicWarpPos(wn, np)) {
                                    tell(pl, "§a已修改公共传送点 §e" + wn + "§a 坐标为 §e(" +
                                             std::to_string((int)np.x) + ", " +
                                             std::to_string((int)np.y) + ", " +
                                             std::to_string((int)np.z) + ") §7" + dimName(np.dimid));
                                } else {
                                    tell(pl, "§c修改失败");
                                }
                            },
                            [&player](Player& pl) { openPublicWarpManageMenu(pl); });
                    });
                // 删除
                fm2.appendButton("§c删除此传送点\n§7不可恢复", tex::DELETE, "path",
                    [&player, warpName](Player&) {
                        if (WarpManager::getInstance().removePublicWarp(warpName)) {
                            tell(player, "§c已删除公共传送点 §e" + warpName);
                        } else {
                            tell(player, "§c删除失败");
                        }
                    });
                fm2.appendButton("§7返回", tex::BACK, "path",
                    [&player](Player&) { openPublicWarpManageMenu(player); });

                fm2.sendTo(player);
            });
    }
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openAdminSettingsMenu(player); });

    fm.sendTo(player);
}

// 免申请传送点列表
void openNoApprovalWarpsList(Player& player, std::string const& actionStr) {
    auto warps = DataStore::getInstance().getNoApprovalWarps();

    if (warps.empty()) {
        // 即使当前没有任何人公开, 也要给出搜索入口（搜索自带权限范围, 管理员另有需求）
        ll::form::SimpleForm fm0("§l§a免申请传送点",
            "§r暂无免申请传送点\n§7可以搜索其它玩家公开的传送点, 或按名称/拼音查找");
        fm0.appendButton("§b搜索玩家 / 传送点\n§7按玩家名、传送点名、拼音模糊查找", tex::INFO, "path",
            [&player](Player&) { openWarpSearchForm(player); });
        fm0.appendButton("§7返回", tex::BACK, "path",
            [&player](Player&) { openWarpMenu(player); });
        fm0.sendTo(player);
        return;
    }

    // 编号直达（指令名见 Config.commands.noapproval）
    if (!actionStr.empty()) {
        int num = 0;
        try { num = std::stoi(actionStr); } catch (...) { num = 0; }
        if (num >= 1 && num <= (int)warps.size()) {
            auto& warp = warps[num - 1];
            if (!teleportPlayerIfReady(player, Vec3((float)warp.pos.x, (float)warp.pos.y, (float)warp.pos.z),
                                       (::DimensionType)warp.pos.dimid)) {
                tell(player, "§c[传送] §f出生点还在加载中，请稍候再试");
                return;
            }
            tell(player, "§a[免申请传送] §f已传送到 §e" + warp.name);
            return;
        }
        tell(player, "§c[免申请传送] §f无效编号 §e" + actionStr + "§f，可用范围 §a1~" +
             std::to_string(warps.size()) + "§f（§e/" + Config::getInstance().getCommand("noapproval") + " 编号§f 直达）");
        return;
    }

    ll::form::SimpleForm fm("§l§a免申请传送点",
        "§r共有 §e" + std::to_string(warps.size()) + "§r 个免申请传送点\n§7点击 §a直接传送§7（无需申请）\n§7快捷指令: §e/" +
        Config::getInstance().getCommand("noapproval") + " 编号§r 可直达对应传送点");

    for (auto& warp : warps) {
        fm.appendButton("§e" + warp.name + "\n§7" + dimName(warp.pos.dimid) + " | 点击传送",
            tex::TELEPORT, "path", [&player, warp](Player&) {
                if (!teleportPlayerIfReady(player, Vec3((float)warp.pos.x, (float)warp.pos.y, (float)warp.pos.z),
                                           (::DimensionType)warp.pos.dimid)) {
                    tell(player, "§c[传送] §f出生点还在加载中，请稍候再试");
                    return;
                }
                tell(player, "§a[免申请传送] §f已传送到 §e" + warp.name);
            });
    }
    fm.appendButton("§b搜索玩家 / 传送点\n§7按玩家名、传送点名、拼音模糊查找", tex::INFO, "path",
        [&player](Player&) { openWarpSearchForm(player); });
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openWarpMenu(player); });

    fm.sendTo(player);
}

// 玩家传送点浏览（浏览他人共享点）
void openPublicWarpBrowser(Player& player) {
    auto& ds = DataStore::getInstance();
    auto const& all = ds.getAllPrivateWarps();

    // 收集开启了免申请的玩家
    struct Entry { std::string xuid, name; int count; };
    std::vector<Entry> entries;

    auto const nameByXuid = onlineNamesByXuid();
    for (auto& [xuid, warps] : all) {
        int noApprCount = 0;
        for (auto& w : warps) if (w.noApproval) noApprCount++;
        if (noApprCount == 0) continue;
        entries.push_back({xuid, nameOfXuid(nameByXuid, xuid), noApprCount});
    }

    if (entries.empty()) {
        tell(player, "§c暂无玩家共享传送点！");
        return openWarpMenu(player);
    }

    ll::form::SimpleForm fm("§l§d玩家传送点浏览",
        "§r选择玩家查看其共享的传送点");

    for (auto& e : entries) {
        fm.appendButton("§e" + e.name + "\n§7共享 " + std::to_string(e.count) + " 个传送点",
            tex::PUBLIC, "path", [&player, e](Player&) {
                openPlayerWarpDetail(player, e.xuid);
            });
    }
    fm.appendButton("§b搜索玩家 / 传送点\n§7按玩家名或传送点名称模糊查找", tex::INFO, "path",
        [&player](Player&) { openWarpSearchForm(player); });
    fm.appendButton("§9所有玩家传送点\n§7列出全部有传送点的玩家（含离线）", tex::PLAYER, "path",
        [&player](Player&) { openAllPlayerWarpsMenu(player); });
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openWarpMenu(player); });

    fm.sendTo(player);
}


// 管理员: 修改某个玩家（xuid）的某个传送点（填入坐标 / 删除）
static void openAdminWarpEditForm(Player& player, std::string const& xuid, std::string const& warpName) {
    ll::form::SimpleForm fm("§l§4管理传送点: " + warpName,
        "§r归属玩家: §f" + nameOfXuid(onlineNamesByXuid(), xuid) +
        "\n§7此页仅管理员可见（模糊搜索进来自带更高一级权限）");

    fm.appendButton("§e填入坐标修改\n§7输入框默认你的当前坐标", tex::SETTINGS, "path",
        [xuid, warpName](Player& p2) {
            std::string xu = xuid, wn = warpName;
            openCoordEditForm(
                p2, "填入坐标修改: " + wn,
                [xu, wn](Player& p3, WarpPos const& np) {
                    bool ok = DataStore::getInstance().mutatePrivateWarp(xu, wn,
                                                                         [&](PrivateWarp& w) { w.pos = np; });
                    tell(p3, ok ? ("§a已修改 §e" + wn + "§a 坐标为 §e(" + std::to_string((int)np.x) + ", " +
                                   std::to_string((int)np.y) + ", " + std::to_string((int)np.z) + ") §7" +
                                   dimName(np.dimid))
                                : "§c修改失败");
                },
                [xu](Player& p3) { openPlayerWarpDetail(p3, xu); });
        });
    fm.appendButton("§c删除此传送点\n§7不可恢复", tex::DELETE, "path",
        [xuid, warpName](Player& p2) {
            if (DataStore::getInstance().removePrivateWarp(xuid, warpName)) {
                tell(p2, "§c已删除 §e" + warpName);
            } else {
                tell(p2, "§c删除失败");
            }
            openPlayerWarpDetail(p2, xuid);
        });
    fm.appendButton("§7返回", tex::BACK, "path", [xuid](Player& p2) { openPlayerWarpDetail(p2, xuid); });

    fm.sendTo(player);
}

// 模糊搜索: 按 玩家名 / 传送点名称 / xuid 子串查找（不区分大小写）
static bool fuzzyContains(std::string const& haystack, std::string const& needle) {
    if (needle.empty()) return true;
    auto lower = [](std::string v) {
        for (auto& c : v) c = (char)std::tolower((unsigned char)c);
        return v;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

void openWarpSearchForm(Player& player) {
    ll::form::CustomForm form("§l§b搜索玩家 / 传送点");
    form.appendInput("kw", "§e关键词", "玩家名 或 传送点名的一部分", "");
    form.appendLabel("§7支持模糊匹配（大小写不敏感）: 玩家名 / 传送点名称 / xuid");
    form.sendTo(player, [](Player& pl, ll::form::CustomFormResult const& res, ll::form::FormCancelReason) {
        if (!res.has_value()) return openNoApprovalWarpsList(pl);
        std::string kw = formGetString(res, "kw");
        // 去首尾空白
        size_t b = kw.find_first_not_of(" \t\r\n");
        size_t e = kw.find_last_not_of(" \t\r\n");
        kw = (b == std::string::npos) ? std::string{} : kw.substr(b, e - b + 1);
        if (kw.empty()) {
            tell(pl, "§c关键词不能为空");
            return openWarpSearchForm(pl);
        }

        auto const  nameByXuid = onlineNamesByXuid();
        auto const& all        = DataStore::getInstance().getAllPrivateWarps();

        // 权限范围: 管理员可搜全部（公开 + 私人）; 普通玩家只搜"公开共享"（免申请）的传送点
        bool const admin = isOp(pl);

        // 关键词的拼音形式（表未加载时为空串, 自动退化为纯文本匹配）
        std::string const kwPy  = pinyinFull(kw);
        std::string const kwIni = pinyinInitials(kw);
        auto matchText = [&](std::string const& s) -> bool {
            if (fuzzyContains(s, kw)) return true;
            if (kwPy.empty() && kwIni.empty()) return false;
            std::string const py  = pinyinFull(s);
            std::string const ini = pinyinInitials(s);
            return (!py.empty() && py.find(kwPy) != std::string::npos) ||
                   (!ini.empty() && !kwIni.empty() && ini.find(kwIni) != std::string::npos);
        };

        struct Hit { std::string xuid, name; int count; std::string hitWarp; };
        std::vector<Hit> hits;
        for (auto& [xuid, warps] : all) {
            std::string name = nameOfXuid(nameByXuid, xuid);
            bool nameHit     = matchText(name) || fuzzyContains(xuid, kw);

            int         visible = 0;
            std::string warpHit;
            for (auto& w : warps) {
                if (!admin && !w.noApproval) continue;   // 普通玩家看不到私人点
                visible++;
                if (warpHit.empty() && matchText(w.name)) warpHit = w.name;
            }
            if (visible == 0) continue;
            if (nameHit || !warpHit.empty()) {
                hits.push_back({xuid, name, visible, warpHit});
            }
        }

        if (hits.empty()) {
            tell(pl, "§c没有找到匹配 §e" + kw + "§c 的玩家或传送点");
            return openWarpSearchForm(pl);
        }

        ll::form::SimpleForm fm("§l§b搜索结果",
            "§r关键词: §e" + kw + "§r\n§7匹配到 §e" + std::to_string(hits.size()) + "§7 位玩家（共 " +
            std::to_string(all.size()) + " 位有传送点）");
        for (auto& h : hits) {
            std::string sub  = h.hitWarp.empty() ? std::string{} : (" §8· 命中传送点: §f" + h.hitWarp);
            std::string xuid = h.xuid;   // 值捕获, 避免引用外层 player
            fm.appendButton("§e" + h.name + "\n§7拥有 " + std::to_string(h.count) + " 个传送点" + sub,
                tex::PLAYER, "path", [xuid](Player& p2) { openPlayerWarpDetail(p2, xuid); });
        }
        fm.appendButton("§e重新搜索\n§7换个关键词", tex::INFO, "path",
            [](Player& p2) { openWarpSearchForm(p2); });
        fm.appendButton("§7返回", tex::BACK, "path",
            [](Player& p2) { openNoApprovalWarpsList(p2); });
        fm.sendTo(pl);
    });
}

// 某玩家的传送点详情（玩家浏览共享点 / 管理员查看所有）
void openPlayerWarpDetail(Player& player, std::string const& xuid) {
    auto& ds = DataStore::getInstance();
    auto const& warps = ds.getPrivateWarps(xuid);

    if (warps.empty()) {
        tell(player, "§c该玩家暂无传送点");
        return;
    }

    // 尝试取玩家名
    std::string name = nameOfXuid(onlineNamesByXuid(), xuid);

    bool admin = isOp(player);

    ll::form::SimpleForm fm("§l§d" + name + " 的传送点",
        "§r玩家 §e" + name + "§r 共有 §e" + std::to_string(warps.size()) + "§r 个传送点\n" +
        (admin ? "§7管理员: 点击传送点可传送过去" : "§7点击共享传送点可传送"));

    for (auto& warp : warps) {
        // 普通玩家只能看到免申请的; 管理员可以看到全部
        if (!admin && !warp.noApproval) continue;

        std::string tag = warp.noApproval ? "§a[免] " : (admin ? "§7[需申请] " : "");
        fm.appendButton(tag + "§e" + warp.name + "\n§7" + dimName(warp.pos.dimid) +
                        " §8(" + std::to_string((int)warp.pos.x) + ", " + std::to_string((int)warp.pos.y) +
                        ", " + std::to_string((int)warp.pos.z) + ")",
            tex::TELEPORT, "path", [&player, warp, admin](Player&) {
                if (!teleportPlayerIfReady(player, Vec3((float)warp.pos.x, (float)warp.pos.y, (float)warp.pos.z),
                                           (::DimensionType)warp.pos.dimid)) {
                    tell(player, "§c[传送] §f出生点还在加载中，请稍候再试");
                    return;
                }
                tell(player, std::string(admin ? "§a[管理] §f已传送到 " : "§a[传送] §f已传送到 ") + "§e" + warp.name);
            });
        if (admin) {
            std::string wn = warp.name;
            fm.appendButton("§4[管理] " + wn + "\n§7填入坐标修改 / 删除此传送点", tex::SETTINGS, "path",
                [xuid, wn](Player& p2) { openAdminWarpEditForm(p2, xuid, wn); });
        }
    }
    fm.appendButton("§7返回", tex::BACK, "path", [](Player& p2) { openWarpMenu(p2); });

    fm.sendTo(player);
}

// [新增] 管理员查看所有玩家的传送点
void openAllPlayerWarpsMenu(Player& player) {
    // 所有玩家都可浏览（详情页 openPlayerWarpDetail 对非管理员只展示"免申请"点, 不泄露私人点）
    auto& ds = DataStore::getInstance();
    auto const& all = ds.getAllPrivateWarps();

    if (all.empty()) {
        tell(player, "§c暂无任何玩家的传送点数据");
        return openWarpMenu(player);
    }

    ll::form::SimpleForm fm("§l§9所有玩家传送点",
        "§r共有 §e" + std::to_string(all.size()) + "§r 位玩家有传送点\n§7点击玩家查看详情并传送");

    auto const nameByXuid = onlineNamesByXuid();
    for (auto& [xuid, warps] : all) {
        std::string name = nameOfXuid(nameByXuid, xuid);
        fm.appendButton("§e" + name + "\n§7拥有 " + std::to_string(warps.size()) + " 个传送点",
            tex::PLAYER, "path", [&player, xuid](Player&) {
                openPlayerWarpDetail(player, xuid);
            });
    }
    fm.appendButton("§7返回", tex::BACK, "path",
        [&player](Player&) { openAdminSettingsMenu(player); });

    fm.sendTo(player);
}

} // namespace menu
} // namespace mtps
