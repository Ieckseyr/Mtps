#pragma once

#include "DataTypes.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <ctime>

class Player;

namespace mtps {

class TpaRally {
public:
    static TpaRally& getInstance();

    // TPA
    bool sendTpaRequest(Player& from, std::string const& toName, std::string const& type, int durationSeconds = 0); // tpa / tpahere
    // 以下三个的第二个参数是"要处理哪一条": 菜单里按条目传入, 免得点第二条却处理了第一条;
    // 留空则取最早的一条（/y、/n 这类不指定条目的入口走这条路径）
    bool acceptRequest(Player& player, std::string const& fromName = {});
    bool refuseRequest(Player& player, std::string const& fromName = {});
    bool cancelRequest(Player& player, std::string const& toName = {});

    // Rally
    bool startRally(Player& player);
    bool joinRally(Player& player);
    bool cancelRally(Player& player);   // 发起者取消（响应过的人会收到通知）
    bool isRallyActive() const;

    // 请求管理
    struct ActiveRequest {
        std::string fromName;
        std::string toName;
        std::string type;       // tpa / tpahere / rally
        int64_t timestamp;
        int duration;
        int joiners{0};         // 仅 rally 用: 已响应人数（发起者看到的那条）
    };
    // 该玩家的待处理请求 + 当前召集的投影（召集不逐人存状态, 见实现里的说明）
    std::vector<ActiveRequest> getRequestsFor(std::string const& playerName) const;

    // 供 PAPI 占位符 mtps_tpa 使用: 返回该玩家当前待处理 TPA 请求的提示文本
    // （无待处理请求 / 已过期 → 空串; 渲染格式取 Config.notification.sidebar.format,
    //   与 JS 版 registerBetterSidebarExtension + getTpaNotificationText 一致）
    std::string notificationTextFor(std::string const& playerName) const;
    // 取该玩家最近一条未过期的待处理请求（谁发的 + 类型文案）; 无则返回 false
    bool pendingIncoming(std::string const& playerName, std::string& fromName, std::string& typeText) const;

    // tick：清理过期请求、召集超时
    void tick();

private:
    TpaRally() = default;

    // 该玩家"够得着"当前召集吗: 召集进行中、不是发起者、没把发起者拉黑、没开拒绝所有传送
    // （与 JS 版 startRally 通知时的筛选条件一致）
    bool rallyReachable(std::string const& playerName) const;
    // 结束当前召集; announce 非空时通知除发起者与 exceptPlayer 外的所有人（取消 / 离线共用）
    void stopRally(std::string const& announce, std::string const& exceptPlayer = {});

    std::vector<ActiveRequest> mRequests;

    // Rally
    bool        mRallyActive{false};
    std::string mRallyStarter;
    double      mRallyX{0}, mRallyY{0}, mRallyZ{0};
    int         mRallyDim{0};
    int64_t     mRallyStartTime{0};
    int         mRallyDuration{60};
    int64_t     mRallyCooldownEnd{0};
    int         mRallyJoiners{0};
};

} // namespace mtps
