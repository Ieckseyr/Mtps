#include "WarpManager.h"
#include "DataStore.h"
#include "Config.h"
#include "Economy.h"
#include "TpUtil.h"

#include <ll/api/service/Bedrock.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <mc/deps/core/math/Vec3.h>

#include <ctime>
#include <unordered_map>

namespace mtps {

WarpManager& WarpManager::getInstance() {
    static WarpManager instance;
    return instance;
}

static WarpPos posFromPlayer(Player& p) {
    auto v = p.getPosition();
    WarpPos pos;
    pos.x = v.x; pos.y = v.y; pos.z = v.z;
    pos.dimid = (int)p.getDimensionId();
    return pos;
}

// 公共传送点
bool WarpManager::addPublicWarp(Player& player, std::string const& name) {
    if (name.empty()) return false;
    PublicWarp w;
    w.name = name;
    w.pos = posFromPlayer(player);
    w.owner = player.getRealName();
    w.enabled = true;
    return DataStore::getInstance().addPublicWarp(w);
}

bool WarpManager::removePublicWarp(std::string const& name) {
    return DataStore::getInstance().removePublicWarp(name);
}

bool WarpManager::teleportToPublicWarp(Player& player, std::string const& name) {
    for (auto& w : DataStore::getInstance().getPublicWarps()) {
        if (w.name == name && w.enabled) {
            int cost = Config::getInstance().getCost("publicWarp");
            if (cost > 0 && Config::getInstance().economyEnabled()) {
                if (!Economy::getInstance().canAfford(player, cost)) {
                    player.sendMessage("§c[传送点] §f余额不足");
                    return false;
                }
                Economy::getInstance().withdraw(player, cost);
            }
            if (!teleportPlayerIfReady(player, Vec3((float)w.pos.x, (float)w.pos.y, (float)w.pos.z),
                                       (::DimensionType)w.pos.dimid)) {
                player.sendMessage("§c[传送] §f出生点还在加载中，请稍候再试");
                return false;
            }
            player.sendMessage("§a[传送点] §f已传送到 §e" + name);
            return true;
        }
    }
    return false;
}

// 新增：管理员修改公共传送点坐标
bool WarpManager::modifyPublicWarpPos(std::string const& name, WarpPos const& pos) {
    return DataStore::getInstance().updatePublicWarpPos(name, pos);
}

bool WarpManager::modifyPublicWarpPosHere(Player& player, std::string const& name) {
    return modifyPublicWarpPos(name, posFromPlayer(player));
}

// 私人传送点
bool WarpManager::addPrivateWarp(Player& player, std::string const& name) {
    std::string xuid = player.getXuid();
    if (DataStore::getInstance().getPrivateWarpCount(xuid) >= Config::getInstance().maxPrivateWarps()) {
        player.sendMessage("§c[私人传送点] §f已达上限 (" + std::to_string(Config::getInstance().maxPrivateWarps()) + ")");
        return false;
    }
    PrivateWarp w;
    w.name = name;
    w.pos = posFromPlayer(player);
    w.createdAt = (int64_t)std::time(nullptr);
    w.noApproval = false;
    return DataStore::getInstance().addPrivateWarp(xuid, w);
}

bool WarpManager::removePrivateWarp(Player& player, std::string const& name) {
    return DataStore::getInstance().removePrivateWarp(player.getXuid(), name);
}

bool WarpManager::teleportToPrivateWarp(Player& player, std::string const& name) {
    auto& warps = DataStore::getInstance().getPrivateWarps(player.getXuid());
    for (auto& w : warps) {
        if (w.name == name) {
            int cost = Config::getInstance().getCost("privateWarp");
            if (cost > 0 && Config::getInstance().economyEnabled()) {
                if (!Economy::getInstance().canAfford(player, cost)) {
                    player.sendMessage("§c[传送点] §f余额不足");
                    return false;
                }
                Economy::getInstance().withdraw(player, cost);
            }
            if (!teleportPlayerIfReady(player, Vec3((float)w.pos.x, (float)w.pos.y, (float)w.pos.z),
                                       (::DimensionType)w.pos.dimid)) {
                player.sendMessage("§c[传送] §f出生点还在加载中，请稍候再试");
                return false;
            }
            player.sendMessage("§a[私人传送点] §f已传送到 §e" + name);
            return true;
        }
    }
    return false;
}

// 新增：玩家重设自己的传送点坐标到当前位置
bool WarpManager::resetPrivateWarpPosHere(Player& player, std::string const& name) {
    bool ok = DataStore::getInstance().updatePrivateWarpPos(player.getXuid(), name, posFromPlayer(player));
    if (ok) player.sendMessage("§a[私人传送点] §f传送点 §e" + name + " §f坐标已更新到当前位置");
    return ok;
}

// 免申请
bool WarpManager::toggleNoApproval(Player& player, std::string const& name, bool& newStateOut) {
    // 原实现用 const_cast 改 getPrivateWarps 返回的 const 引用（UB）且绕过了 DataStore 的锁;
    // 且把"切换后的状态"当成功标志返回 —— 关掉开关会被上层当成失败。改为 true=找到并已切换。
    bool toggledTo = false;
    bool found = DataStore::getInstance().mutatePrivateWarp(
        player.getXuid(), name,
        [&toggledTo](PrivateWarp& w) { w.noApproval = !w.noApproval; toggledTo = w.noApproval; });
    newStateOut = toggledTo;
    return found;
}

std::vector<PrivateWarp> WarpManager::getNoApprovalWarps() const {
    return DataStore::getInstance().getNoApprovalWarps();
}

// 管理员查看所有玩家传送点
std::vector<WarpManager::PlayerWarpInfo> WarpManager::getAllPlayerWarps() const {
    std::vector<PlayerWarpInfo> result;
    // 一次遍历建 xuid → 名字映射（原实现对每个 xuid 都跑一遍 forEachPlayer: O(N×M)）
    std::unordered_map<std::string, std::string> nameByXuid;
    if (auto level = ll::service::getLevel()) {
        level->forEachPlayer([&](Player& p) -> bool { nameByXuid[p.getXuid()] = p.getRealName(); return true; });
    }
    auto const& all = DataStore::getInstance().getAllPrivateWarps();
    result.reserve(all.size());
    for (auto& [xuid, warps] : all) {
        PlayerWarpInfo info;
        info.xuid  = xuid;
        info.warps = warps;
        auto it    = nameByXuid.find(xuid);
        info.playerName = (it != nameByXuid.end()) ? it->second : xuid;
        result.push_back(std::move(info));
    }
    return result;
}

bool WarpManager::adminTeleportToPlayerWarp(Player& admin, std::string const& targetXuid, std::string const& warpName) {
    auto& warps = DataStore::getInstance().getPrivateWarps(targetXuid);
    for (auto& w : warps) {
        if (w.name == warpName) {
            if (!teleportPlayerIfReady(admin, Vec3((float)w.pos.x, (float)w.pos.y, (float)w.pos.z),
                                       (::DimensionType)w.pos.dimid)) {
                admin.sendMessage("§c[传送] §f出生点还在加载中，请稍候再试");
                return false;
            }
            admin.sendMessage("§a[管理] §f已传送到玩家传送点 §e" + warpName);
            return true;
        }
    }
    return false;
}

} // namespace mtps

