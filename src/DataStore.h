#pragma once

#include "DataTypes.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mtps {

using json = nlohmann::json;

// 数据存储。落盘策略: 修改只置脏位(mDirty), tick() 到期(默认 3 秒)才合并落盘,
// saveAll() 立即落盘; 写文件走 temp + rename 原子替换, 写一半崩溃不会毁掉原数据。
class DataStore {
public:
    static DataStore& getInstance();

    bool loadAll();
    // 立即把全部脏文件落盘（关服/显式调用）
    bool saveAll();
    // 每 tick 调用: 有脏数据且距上次落盘超过间隔时间则合并落盘
    void tick();

    // 私人传送点
    std::vector<PrivateWarp> const& getPrivateWarps(std::string const& xuid) const;
    bool addPrivateWarp(std::string const& xuid, PrivateWarp const& warp);
    bool removePrivateWarp(std::string const& xuid, std::string const& name);
    bool updatePrivateWarpPos(std::string const& xuid, std::string const& name, WarpPos const& pos);
    int  getPrivateWarpCount(std::string const& xuid) const;
    // 定向修改单个传送点（持锁, 自动置脏）; fn 返回后立即生效
    bool mutatePrivateWarp(std::string const& xuid, std::string const& name,
                           std::function<void(PrivateWarp&)> const& fn);

    // 公共传送点
    std::vector<PublicWarp> const& getPublicWarps() const;
    bool addPublicWarp(PublicWarp const& warp);
    bool removePublicWarp(std::string const& name);
    bool updatePublicWarpPos(std::string const& name, WarpPos const& pos);

    // 免申请传送点
    std::vector<PrivateWarp> getNoApprovalWarps() const;
    // 免申请传送点数量（避免为了显示一个数字做全量拷贝）
    int countNoApprovalWarps() const;

    // btp 传送点
    std::vector<BlockTpPoint> const& getBlockTpPoints() const;
    bool addBlockTpPoint(BlockTpPoint const& point);
    bool removeBlockTpPoint(std::string const& name);
    bool updateBlockTpPoint(BlockTpPoint const& point);
    // 定向修改单个 btp 点（持锁, 自动置脏）
    bool mutateBlockTpPoint(std::string const& name, std::function<void(BlockTpPoint&)> const& fn);

    // TPA 缓存
    void addTpaRequester(std::string const& name);
    void removeTpaRequester(std::string const& name);
    bool hasTpaRequester(std::string const& name) const;

    // 传送点申请
    void addWarpRequest(WarpRequest const& req);
    void removeWarpRequest(std::string const& requester, std::string const& targetPlayer);
    std::vector<WarpRequest> getWarpRequestsFor(std::string const& targetPlayer) const;

    // 个人规则
    PersonalRules getPersonalRules(std::string const& xuid) const;
    // 黑名单（按玩家名存, 与个人设置一致）: 名单里的人不能向我发起传送
    std::vector<std::string> getBlockedPlayers(std::string const& key) const;
    void                     setBlockedPlayers(std::string const& key, std::vector<std::string> const& list);
    void setPersonalRules(std::string const& xuid, PersonalRules const& rules);

    // 跨服传送目的地
    std::vector<CrossServerEntry> const& getCrossServers() const;
    bool addCrossServer(CrossServerEntry const& e);
    bool removeCrossServer(std::string const& name);
    bool updateCrossServer(CrossServerEntry const& e);

    // 所有玩家的私人传送点（管理员查看）
    std::unordered_map<std::string, std::vector<PrivateWarp>> const& getAllPrivateWarps() const;

private:
    DataStore() = default;

    // 数据文件位（脏标记用）
    enum FileBit : uint32_t {
        F_Private  = 1u << 0,
        F_Public   = 1u << 1,
        F_BlockTp  = 1u << 2,
        F_TpaCache = 1u << 3,
        F_WarpReq  = 1u << 4,
        F_Rules    = 1u << 5,
        F_Cross    = 1u << 6,
        F_All      = 0x7Fu,
    };

    static json loadJson(std::string const& path, json const& def);
    // temp + rename 原子写
    static bool saveJson(std::string const& path, json const& data);

    void markDirtyLocked(uint32_t bits);   // 调用方需持 mMutex
    bool flushLocked();                    // 调用方需持 mMutex; 落盘全部脏文件

    mutable std::mutex mMutex;

    std::unordered_map<std::string, std::vector<PrivateWarp>> mPrivateWarps;
    std::vector<PublicWarp>  mPublicWarps;
    std::vector<BlockTpPoint> mBlockTpPoints;
    std::vector<std::string>  mTpaRequesters;
    std::vector<WarpRequest>  mWarpRequests;
    std::vector<CrossServerEntry> mCrossServers;
    std::unordered_map<std::string, PersonalRules> mPersonalRules;

    uint32_t mDirty{0};          // 脏文件位图
    int64_t  mLastFlushMs{0};    // 上次落盘时刻（steady clock 毫秒）
};

} // namespace mtps
