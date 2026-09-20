#include "TpUtil.h"

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/Bedrock.h>

#include <mc/server/ServerLevel.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/PlayerDimensionTransferManager.h>

#include <climits>
#include <unordered_map>
#include <unordered_set>

namespace mtps {

namespace {

// 换维度后这段时间内仍然算"切换中": 引擎那边区域已经恢复, 但 ChangeDimension 包刚发出去、
// 客户端还在加载画面 —— 这段时间传送照样会出幽灵状态（也就是刷物窗口）。
// 想改宽松/严格改这一个数就行（20 tick = 1 秒）。
constexpr int64_t kDimSettleTicks = 100;
constexpr int64_t kPollEveryTicks = 10;    // 每 0.5 秒扫一遍在线玩家, 看谁的维度号变了

int64_t sTick = 0;

struct DimMark {
    int     dim{INT_MIN};
    int64_t changedAt{0};
};
std::unordered_map<std::string, DimMark> sDimMarks;

// 只在"管理器说在切换、但区域没挂起也没观察到维度变化"时提示一次 —— 这种组合有可能说明
// 管理器的判定另有含义（那就会误挡传送）, 留个日志方便定位
std::unordered_set<std::string> sManagerOnlyWarned;

} // namespace

// 每 tick 调一次（Mtps 的 ServerLevelTickEvent）
void tpGuardTick() {
    ++sTick;
    if (sTick % kPollEveryTicks != 0) return;
    auto level = ll::service::getLevel();
    if (!level) {
        sDimMarks.clear();
        return;
    }
    level->forEachPlayer([&](Player& p) -> bool {
        std::string const name = p.getRealName();
        int const         dim  = (int)p.getDimensionId();
        auto              it   = sDimMarks.find(name);
        if (it == sDimMarks.end()) {
            // 首次见到（刚上线/插件刚启动）: 不算"刚换过维度", 否则每个人上线头 5 秒都不能传
            sDimMarks[name] = DimMark{dim, sTick - kDimSettleTicks - 1};
        } else if (it->second.dim != dim) {
            it->second.dim       = dim;
            it->second.changedAt = sTick;
        }
        return true;
    });
    // 离线玩家清掉, 免得名字表一直涨
    if (sDimMarks.size() > 64) {
        for (auto i = sDimMarks.begin(); i != sDimMarks.end();) {
            i = level->getPlayer(i->first) ? std::next(i) : sDimMarks.erase(i);
        }
    }
}

bool isPlayerInDimensionTransfer(Player const& player) {
    // ① 区域被挂起 = 引擎正在给这个玩家换维度: 客户端还在加载画面, 服务端这边玩家的区块视图
    //    已经拆掉了。这期间传送会撕出幽灵玩家（老实体留在原维度）→ 可刷物。
    bool const regionSuspended = (bool)player.mIsRegionSuspended;

    // ③ 观察到这个玩家的维度号刚变过 → 客户端还在加载新维度
    bool dimJustChanged = false;
    auto it             = sDimMarks.find(player.getRealName());
    if (it != sDimMarks.end()) dimJustChanged = (sTick - it->second.changedAt) < kDimSettleTicks;

    // ② 换维度请求已经排队、区域还没挂起的那几拍: 问管理器（它管着换维度请求队列）
    bool managerSuspended = false;
    if (auto level = ll::service::getLevel()) {
        try {
            managerSuspended = level->getPlayerDimensionTransferManager()->_isPlayerSuspended(player);
        } catch (...) {
            managerSuspended = false;
        }
    }
    if (managerSuspended && !regionSuspended && !dimJustChanged) {
        if (sManagerOnlyWarned.insert(player.getRealName()).second) {
            ll::mod::NativeMod::current()->getLogger().warn(
                "[传送保护] 玩家 {} 被判为正在切换维度（管理器判定; 区域未挂起、也没观察到维度变化）"
                " —— 若发现传送被无故拦住, 把这条日志发出来", player.getRealName());
        }
    }
    return regionSuspended || dimJustChanged || managerSuspended;
}

std::string tpBlockedText(Player const& player) {
    if (isPlayerInDimensionTransfer(player)) return "§f正在切换维度，等切换结束后再试";
    if (!isPlayerSpawned(player))           return "§f出生点还在加载中，请稍候再试";
    return {};
}

bool teleportPlayerIfReady(Player& player, Vec3 const& pos, DimensionType dim) {
    if (!isPlayerSpawned(player)) return false;
    if (isPlayerInDimensionTransfer(player)) return false;
    player.teleport(pos, dim);
    return true;
}

} // namespace mtps
