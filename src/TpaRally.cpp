#include "TpaRally.h"
#include "DataStore.h"
#include "Config.h"
#include "Economy.h"
#include "TpUtil.h"

#include <ll/api/service/Bedrock.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <mc/deps/core/math/Vec3.h>

#include <algorithm>

namespace mtps {

TpaRally& TpaRally::getInstance() {
    static TpaRally instance;
    return instance;
}

bool TpaRally::sendTpaRequest(Player& from, std::string const& toName, std::string const& type,
                              int durationSeconds) {
    auto level = ll::service::getLevel();
    if (!level) return false;
    Player* to = level->getPlayer(toName);
    if (!to) {
        from.sendMessage("§c[TPA] §f玩家不在线");
        return false;
    }
    if (to->getRealName() == from.getRealName()) {
        from.sendMessage("§c[TPA] §f不能传送给自己");
        return false;
    }
    // 黑名单: 对方把我拉黑了就直接拒绝
    {
        auto blk = DataStore::getInstance().getBlockedPlayers(to->getRealName());
        if (std::find(blk.begin(), blk.end(), from.getRealName()) != blk.end()) {
            from.sendMessage("§c[TPA] §f对方已屏蔽你的传送请求");
            return false;
        }
    }

    // 检查目标个人规则
    auto rules = DataStore::getInstance().getPersonalRules(to->getRealName());
    if (rules.refuseAllTpaRequest) {
        from.sendMessage("§c[TPA] §f对方已拒绝所有传送请求");
        return false;
    }

    // 经济
    int cost = Config::getInstance().getCost("tpa");
    if (cost > 0 && Config::getInstance().economyEnabled()) {
        if (!Economy::getInstance().canAfford(from, cost)) {
            from.sendMessage("§c[TPA] §f余额不足");
            return false;
        }
    }

    ActiveRequest req;
    req.fromName = from.getRealName();
    req.toName = to->getRealName();
    req.type = type;
    req.timestamp = (int64_t)std::time(nullptr);
    // 表单里填的时长优先; 没给就用配置值
    req.duration = durationSeconds > 0 ? durationSeconds : Config::getInstance().effectiveDuration();
    mRequests.push_back(req);

    std::string typeText = (type == "tpahere") ? "想让你传送到TA身边" : "想传送到你身边";
    auto&       cfg      = Config::getInstance();
    to->sendMessage("§e" + from.getRealName() + "§r " + typeText + "，使用 §a/" + cfg.getCommand("accept") +
                    " §f同意 §c/" + cfg.getCommand("refuse") + " §f拒绝");
    from.sendMessage("§a[TPA] §f请求已发送给 §e" + toName);
    return true;
}

bool TpaRally::acceptRequest(Player& player, std::string const& fromName) {
    std::string name = player.getRealName();
    auto it = std::find_if(mRequests.begin(), mRequests.end(),
        [&](ActiveRequest const& r) {
            if (r.toName != name) return false;
            return fromName.empty() || r.fromName == fromName;
        });
    if (it == mRequests.end()) {
        // 没有 TPA 请求时, 同意(/y) 就是响应召集 —— 召集不再单独占一条指令
        if (fromName.empty() && isRallyActive() && rallyReachable(player.getRealName())) {
            return joinRally(player);
        }
        player.sendMessage("§c[TPA] §f没有待处理的请求");
        return false;
    }

    auto level = ll::service::getLevel();
    if (!level) return false;
    Player* from = level->getPlayer(it->fromName);
    if (!from) {
        player.sendMessage("§c[TPA] §f请求发起者已离线");
        mRequests.erase(it);
        return false;
    }

    // 经济扣费
    int cost = Config::getInstance().getCost("tpa");
    if (cost > 0 && Config::getInstance().economyEnabled()) {
        Economy::getInstance().withdraw(*from, cost);
    }

    if (it->type == "tpa") {
        // from 传送到 player
        if (!teleportPlayerIfReady(*from, player.getPosition(), player.getDimensionId())) {
            from->sendMessage("§c[传送] §f出生点还在加载中，请稍候再试");
            player.sendMessage("§c[TPA] §f对方出生点还在加载中，请稍候再试");
            mRequests.erase(it);
            return false;
        }
        from->sendMessage("§a[TPA] §f已传送到 §e" + name);
        player.sendMessage("§a[TPA] §f" + it->fromName + " 已传送到你身边");
    } else {
        // tpahere: player 传送到 from
        if (!teleportPlayerIfReady(player, from->getPosition(), from->getDimensionId())) {
            player.sendMessage("§c[传送] §f出生点还在加载中，请稍候再试");
            mRequests.erase(it);
            return false;
        }
        player.sendMessage("§a[TPA] §f已传送到 §e" + it->fromName + " §f身边");
        from->sendMessage("§a[TPA] §f" + name + " 已传送到你身边");
    }
    mRequests.erase(it);
    return true;
}

bool TpaRally::refuseRequest(Player& player, std::string const& fromName) {
    std::string name = player.getRealName();
    auto it = std::find_if(mRequests.begin(), mRequests.end(),
        [&](ActiveRequest const& r) {
            if (r.toName != name) return false;
            return fromName.empty() || r.fromName == fromName;
        });
    if (it == mRequests.end()) {
        player.sendMessage("§c[TPA] §f没有待处理的请求");
        return false;
    }
    auto level = ll::service::getLevel();
    if (level) {
        Player* from = level->getPlayer(it->fromName);
        if (from) from->sendMessage("§c[TPA] §f" + name + " 拒绝了你的传送请求");
    }
    player.sendMessage("§c[TPA] §f已拒绝传送请求");
    mRequests.erase(it);
    return true;
}

bool TpaRally::cancelRequest(Player& player, std::string const& toName) {
    std::string name = player.getRealName();
    auto it = std::find_if(mRequests.begin(), mRequests.end(),
        [&](ActiveRequest const& r) {
            if (r.fromName != name) return false;
            return toName.empty() || r.toName == toName;
        });
    if (it == mRequests.end()) {
        player.sendMessage("§c[TPA] §f你没有待处理的请求");
        return false;
    }
    mRequests.erase(it);
    player.sendMessage("§a[TPA] §f已取消传送请求");
    return true;
}

// PAPI 占位符 mtps_tpa 的取值逻辑（对齐 JS 版 getTpaNotificationText）:
//   取该玩家最近一条未过期的"待自己处理"的 TPA 请求, 按 sidebar.format 渲染; 无则空串
bool TpaRally::pendingIncoming(std::string const& playerName, std::string& fromName,
                               std::string& typeText) const {
    int64_t const now = (int64_t)std::time(nullptr);
    ActiveRequest const* best = nullptr;
    for (auto& r : mRequests) {
        if (r.toName != playerName) continue;                  // 只看"别人请求我"的
        if (now - r.timestamp > r.duration) continue;          // 已过期
        if (!best || r.timestamp > best->timestamp) best = &r;  // 取最新一条
    }
    if (!best) return false;
    fromName = best->fromName;
    typeText = (best->type == "tpahere") ? "想让你传送到TA身边" : "想传送到你身边";
    return true;
}

std::string TpaRally::notificationTextFor(std::string const& playerName) const {
    if (!Config::getInstance().sidebarEnabled()) return "";

    std::string fromName, typeText;
    if (!pendingIncoming(playerName, fromName, typeText)) return "";
    std::string       out      = Config::getInstance().sidebarFormat();
    auto replaceAll = [](std::string& str, std::string const& from, std::string const& to) {
        if (from.empty()) return;
        for (size_t pos = 0; (pos = str.find(from, pos)) != std::string::npos; pos += to.size()) {
            str.replace(pos, from.size(), to);
        }
    };
    replaceAll(out, "{player}", fromName);
    replaceAll(out, "{type}", typeText);
    return out;
}

std::vector<TpaRally::ActiveRequest> TpaRally::getRequestsFor(std::string const& playerName) const {
    std::vector<ActiveRequest> result;
    for (auto& r : mRequests) {
        if (r.toName == playerName || r.fromName == playerName) result.push_back(r);
    }
    // 召集对整个服务器只存在一个, 所以这里按"当前召集"投影出一条, 而不是给每个玩家各存一条:
    // 存的话就有两份状态要对齐（谁响应过、谁走掉了、召集提前结束了）, 投影不会不同步。
    // 发起者看到自己那条（点击取消召集）, 够得着的其他人看到邀请（点击加入）。
    if (mRallyActive && (playerName == mRallyStarter || rallyReachable(playerName))) {
        ActiveRequest r;
        r.fromName  = mRallyStarter;
        r.toName    = mRallyStarter;        // 发起者那条 toName 是自己, 菜单据 fromName 判断归属
        r.type      = "rally";
        r.timestamp = mRallyStartTime;
        r.duration  = mRallyDuration;
        r.joiners   = mRallyJoiners;
        result.insert(result.begin(), r);
    }
    return result;
}

// 该玩家"够得着"当前召集吗: 召集进行中、不是发起者本人、没把发起者拉黑、没开拒绝所有传送
bool TpaRally::rallyReachable(std::string const& playerName) const {
    if (!mRallyActive) return false;
    if (playerName == mRallyStarter) return false;
    auto& ds  = DataStore::getInstance();
    auto  blk = ds.getBlockedPlayers(playerName);
    if (std::find(blk.begin(), blk.end(), mRallyStarter) != blk.end()) return false;
    return !ds.getPersonalRules(playerName).refuseAllTpaRequest;
}

// Rally
bool TpaRally::startRally(Player& player) {
    if (!Config::getInstance().rallyEnabled()) {
        player.sendMessage("§c[召集] §f召集功能未开启");
        return false;
    }
    // 自己发起的召集还在进行 → 再点一次就是取消（与 JS 版 /rally 的行为一致）
    if (mRallyActive && mRallyStarter == player.getRealName()) return cancelRally(player);
    // 别人的召集还在进行 → 不允许同时开两个, 否则会把前一个悄悄顶掉
    if (mRallyActive) {
        player.sendMessage("§c[召集] §f当前已有 §e" + mRallyStarter + "§f 发起的召集，请等待其结束");
        return false;
    }
    int64_t now = (int64_t)std::time(nullptr);
    if (now < mRallyCooldownEnd) {
        player.sendMessage("§c[召集] §f召集冷却中");
        return false;
    }
    mRallyActive = true;
    mRallyStarter = player.getRealName();
    auto pos = player.getPosition();
    mRallyX = pos.x; mRallyY = pos.y; mRallyZ = pos.z;
    mRallyDim = (int)player.getDimensionId();
    mRallyStartTime = now;
    mRallyDuration = Config::getInstance().rallyDuration();
    mRallyCooldownEnd = now + Config::getInstance().rallyCooldownMs() / 1000;
    mRallyJoiners = 0;

    // 通知够得着的玩家（与 JS 版一致: 拉黑了发起者、或开了"拒绝所有传送"的人不打扰）。
    // 这是功能消息不是广播日志, 所以不看 logging.broadcastToGame
    std::string const menuCmd = Config::getInstance().getCommand("menu");
    int notified = 0;
    if (auto level = ll::service::getLevel()) {
        level->forEachPlayer([&](Player& p) -> bool {
            if (!rallyReachable(p.getRealName())) return true;
            p.sendMessage("§6[召集] §e" + mRallyStarter + "§r 发起了召集！打开 §a/" + menuCmd +
                          " §r菜单的§e请求管理§r即可传送到TA身边");
            notified++;
            return true;
        });
    }
    player.sendMessage("§a[召集] §f已发起召集，§e" + std::to_string(mRallyDuration) +
                       "§f 秒内有效（已通知 §e" + std::to_string(notified) + "§f 人）");
    player.sendMessage("§7其他玩家可在 §a/" + menuCmd + " §7→§e请求管理§7 中响应；你在那里点召集条目即可取消");
    return true;
}

bool TpaRally::joinRally(Player& player) {
    if (!mRallyActive) {
        player.sendMessage("§c[召集] §f当前没有进行中的召集");
        return false;
    }
    if (player.getRealName() == mRallyStarter) {
        player.sendMessage("§c[召集] §f不能加入自己发起的召集");
        return false;
    }
    if (!rallyReachable(player.getRealName())) {
        player.sendMessage("§c[召集] §f你无法加入该召集");
        return false;
    }
    // 发起者离线 → "到我身边"已无从谈起, 而且落点可能已经跟着卸载, 直接结束召集
    // （与 JS 版 joinRally 的处理一致）
    auto level = ll::service::getLevel();
    if (!level || !level->getPlayer(mRallyStarter)) {
        player.sendMessage("§c[召集] §f召集发起者已离线，召集已结束");
        stopRally("§6[召集] §e" + mRallyStarter + "§r 已离线，召集传送已结束", player.getRealName());
        return false;
    }
    // 经济: 与 JS 版一致, 由加入者付费（费用为 0 时不检查）
    int cost = Config::getInstance().getCost("rally");
    bool charge = cost > 0 && Config::getInstance().economyEnabled();
    if (charge && !Economy::getInstance().canAfford(player, cost)) {
        player.sendMessage("§c[召集] §f余额不足，需要 §e" + std::to_string(cost) + "§f 才能加入");
        return false;
    }
    if (!teleportPlayerIfReady(player, Vec3((float)mRallyX, (float)mRallyY, (float)mRallyZ),
                               (::DimensionType)mRallyDim)) {
        player.sendMessage("§c[传送] §f出生点还在加载中，请稍候再试");
        return false;
    }
    if (charge) Economy::getInstance().withdraw(player, cost);
    mRallyJoiners++;
    std::string const costTip = cost > 0 ? "§f（花费 §e" + std::to_string(cost) + "§f）" : "";
    player.sendMessage("§a[召集] §f已传送到 §e" + mRallyStarter + " §f身边" + costTip);
    if (Player* starter = level->getPlayer(mRallyStarter)) {
        starter->sendMessage("§e" + player.getRealName() + "§a 响应了你的召集（已响应 §e" +
                             std::to_string(mRallyJoiners) + "§a 人）");
    }
    return true;
}

// 结束当前召集; announce 非空时通知除发起者与 exceptPlayer 外的所有人
void TpaRally::stopRally(std::string const& announce, std::string const& exceptPlayer) {
    if (!mRallyActive) return;
    std::string const who = mRallyStarter;
    mRallyActive = false;
    if (announce.empty()) return;
    if (auto level = ll::service::getLevel()) {
        level->forEachPlayer([&](Player& p) -> bool {
            if (p.getRealName() != who && p.getRealName() != exceptPlayer) p.sendMessage(announce);
            return true;
        });
    }
}

// 发起者取消召集（响应过的人会收到通知）
bool TpaRally::cancelRally(Player& player) {
    if (!mRallyActive || player.getRealName() != mRallyStarter) {
        player.sendMessage("§c[召集] §f当前没有你发起的召集");
        return false;
    }
    player.sendMessage("§a[召集] §f已取消你的召集传送");
    stopRally("§6[召集] §e" + player.getRealName() + "§r 已取消召集传送");
    return true;
}

bool TpaRally::isRallyActive() const { return mRallyActive; }

void TpaRally::tick() {
    int64_t now = (int64_t)std::time(nullptr);
    // 清理过期 TPA 请求
    mRequests.erase(std::remove_if(mRequests.begin(), mRequests.end(),
        [now](ActiveRequest const& r) { return now - r.timestamp > r.duration; }),
        mRequests.end());
    // 召集超时
    if (mRallyActive && now - mRallyStartTime > mRallyDuration) {
        mRallyActive = false;
    }
}

} // namespace mtps
