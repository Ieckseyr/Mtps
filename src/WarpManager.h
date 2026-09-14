#pragma once

#include "DataTypes.h"
#include <string>
#include <vector>

class Player;

namespace mtps {

class WarpManager {
public:
    static WarpManager& getInstance();

    // 公共传送点
    bool addPublicWarp(Player& player, std::string const& name);
    bool removePublicWarp(std::string const& name);
    bool teleportToPublicWarp(Player& player, std::string const& name);
    // 新增：管理员修改公共传送点坐标到指定位置
    bool modifyPublicWarpPos(std::string const& name, WarpPos const& pos);
    // 新增：管理员修改公共传送点坐标到当前玩家位置
    bool modifyPublicWarpPosHere(Player& player, std::string const& name);

    // 私人传送点
    bool addPrivateWarp(Player& player, std::string const& name);
    bool removePrivateWarp(Player& player, std::string const& name);
    bool teleportToPrivateWarp(Player& player, std::string const& name);
    // 新增：玩家将自己的传送点坐标重设到当前位置
    bool resetPrivateWarpPosHere(Player& player, std::string const& name);

    // 免申请传送点
    // 切换免申请开关; 返回 false = 未找到该传送点（注意: 不是"新状态为关"）
    // newStateOut: 切换后的状态
    bool toggleNoApproval(Player& player, std::string const& name, bool& newStateOut);
    std::vector<PrivateWarp> getNoApprovalWarps() const;

    // 新增：管理员查看所有玩家的私人传送点
    struct PlayerWarpInfo {
        std::string xuid;
        std::string playerName;
        std::vector<PrivateWarp> warps;
    };
    std::vector<PlayerWarpInfo> getAllPlayerWarps() const;

    // 管理员传送到某个玩家的私人传送点
    bool adminTeleportToPlayerWarp(Player& admin, std::string const& targetXuid, std::string const& warpName);

private:
    WarpManager() = default;
};

} // namespace mtps
