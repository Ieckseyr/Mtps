#include "RandomTeleportInternal.h"

#include <mc/util/Bounds.h>
#include <mc/world/Pos.h>
#include <mc/world/level/ChunkPos.h>
#include <mc/world/level/chunk/ChunkSource.h>
#include <mc/world/level/dimension/Dimension.h>

#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/Bedrock.h>

#include <mc/deps/core/utility/MCRESULT.h>
#include <mc/server/ServerLevel.h>
#include <mc/server/commands/CommandContext.h>
#include <mc/server/commands/CommandPermissionLevel.h>
#include <mc/server/commands/CurrentCmdVersion.h>
#include <mc/server/commands/MinecraftCommands.h>
#include <mc/server/commands/ServerCommandOrigin.h>
#include <mc/world/Minecraft.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/ticking/ITickingArea.h>
#include <mc/world/level/ticking/PendingArea.h>
#include <mc/world/level/ticking/TickingAreaDescription.h>
#include <mc/world/level/ticking/AddTickingAreaStatus.h>
#include <mc/world/level/ticking/TickingAreaLoadMode.h>
#include <mc/world/level/ticking/TickingAreaList.h>
#include <mc/world/level/ticking/TickingAreasManager.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>

namespace mtps {

static std::atomic<uint64_t> gAreaSeq{0};
std::string makeAreaName(std::string const& playerName) {
    std::string out{RTP_AREA_PREFIX};
    for (char c : playerName) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    out += '_';
    out += std::to_string(gAreaSeq.fetch_add(1));
    return out;
}

// ── 直接请求引擎处理一个区块 ───────────────────────────────────────────────
// 注意 LoadMode: None = 必须立刻拿到（拿不到返回空）, Deferred = 允许异步完成。
// 之前两次都传 None 所以恒返回空 —— 对需要生成的区块那是必然的。
// 返回 true = 引擎已接手（可能是刚生成/正在生成/已在内存）。
bool requestChunkLoad(int dimid, int blockX, int blockZ) {
    auto level = ll::service::getLevel();
    if (!level) return false;
    auto dim = level->getDimension((::DimensionType)dimid).lock();
    if (!dim) return false;

    auto const cp     = ::ChunkPos(blockX >> 4, blockZ >> 4);
    auto&      source = (*dim).getChunkSource();
    if (source.getExistingChunk(cp)) {
        RTP_DBG("[RTP][加载] 区块 ({}, {}) 已在内存", cp.x, cp.z);
        return true;
    }
    auto chunk = source.createNewChunk(cp, ::ChunkSource::LoadMode::Deferred, /*readOnly*/ false);
    RTP_DBG("[RTP][加载] 区块 ({}, {}) → {}", cp.x, cp.z, chunk ? "引擎已接手（Deferred）" : "仍为空");
    return chunk != nullptr;
}

// ── 登记常加载区域（引擎 API, 不经命令）──────────────────────────────────────
// 这就是 /tickingarea 能生成新区块的原因: TickingAreasManager 的活动区域表**引擎每 tick 都会
// 处理**, 区域内的区块会被真正加载并保持常加载 —— 存档里有就从盘载入, 从来没有过就生成。
// getOrLoadChunk / createNewChunk 都不是这条路, 对从未生成过的区块只会返回空。
//
// 与 /tickingarea 命令的区别（也是不用命令的原因）:
//   - 直接调引擎 API, 不经过命令解析;
//   - isPersistent = false: 不写进存档、重启不会被预加载、会话结束即消失;
//   - AreaLimitCheck::None: 跳过"常加载区域个数上限"（临时用途, 用完 removeRtpArea 删掉）。
AddTickingAreaStatus addRtpArea(Level& level, int dimid, std::string const& name,
                                int blockX, int blockZ, int radiusChunks) {
    // 单位注意: Bounds 的 x/z 是**区块**坐标（/tickingarea 的 circle 半径也是区块, 上限 4）, y 是方块。
    // 之前按方块填 (±64) 在区块解释下等于半径 129 区块 = 16641 个区块的巨型区域, 引擎先忙着生成
    // 外围、中心区块迟迟不到 → 表现为"登记成功但一直 Unloaded"。
    int const ccx  = blockX >> 4;
    int const ccz  = blockZ >> 4;
    int const r    = radiusChunks;
    int const side = 2 * r + 1;

    ::Bounds bounds{};
    bounds.mMin    = ::Pos{ccx - r, 64, ccz - r};
    bounds.mMax    = ::Pos{ccx + r, 64, ccz + r};
    bounds.mDim    = ::Pos{side, 1, side};
    bounds.mSide   = side;
    bounds.mArea   = side * side;
    bounds.mVolume = side * side;

    // Preload: 主动把区域内的区块加载起来（Default 只是登记, 不主动加载 —— 这就是
    // 上一版登记成功、区块却一直 Unloaded 的原因）
    return level.getTickingAreasMgr()._addArea(
        (::DimensionType)dimid, name, bounds, /*isCircle*/ true,
        TickingAreasManager::AreaLimitCheck::None, /*isPersistent*/ false,
        ::TickingAreaLoadMode::Preload, level.getLevelStorage());
}

// 诊断: 找到活动区域（读回引擎侧的 bounds / 加载模式 / 加载进度）
ITickingArea* findRtpArea(Level& level, int dimid, std::string const& name) {
    try {
        auto& mgr = level.getTickingAreasMgr();
        auto  it  = mgr.mActiveAreas->find((::DimensionType)dimid);
        if (it == mgr.mActiveAreas->end() || !it->second) return nullptr;
        for (auto& a : *it->second->mTickingAreas) {
            if (a && a->getName() == name) return a.get();
        }
    } catch (...) {}
    return nullptr;
}

// 诊断: 该名字的区域是否还挂在 pending 列表里（没被引擎激活）。
// 用途: 等待超时时打印, 区分"引擎没受理/没激活"和"激活了但生成慢".
bool areaStillPending(Level& level, int dimid, std::string const& name) {
    try {
        auto& mgr = level.getTickingAreasMgr();
        auto  it  = mgr.mPendingAreas->find((::DimensionType)dimid);
        if (it == mgr.mPendingAreas->end()) return false;
        for (auto& pa : it->second) {
            if (auto* n = pa.mName.operator->(); n != nullptr && *n == name) return true;
        }
    } catch (...) {}
    return false;
}

// 移除 RTP 创建的常加载区域（pending + active 双路径都走, 命中即删, 引擎同步删持久化记录）
void removeRtpArea(Level& level, int dimid, std::string const& name) {
    try {
        auto& mgr     = level.getTickingAreasMgr();
        auto& storage = level.getLevelStorage();
        mgr.removePendingAreaByName((::DimensionType)dimid, name, storage);
        // 活动区域兜底（若上一行已处理则此处为空操作）
        auto& active = *mgr.mActiveAreas;
        if (auto it = active.find((::DimensionType)dimid); it != active.end() && it->second) {
            auto areas = it->second->findStandaloneAreasNamed(name);
            if (!areas.empty()) it->second->removeAreas(areas, storage);
        }
    } catch (std::exception const& e) {
        RTP_DBG("[RTP][区域] 移除 {} 失败: {}", name, e.what());
    } catch (...) {
        RTP_DBG("[RTP][区域] 移除 {} 失败: 未知异常", name);
    }
}

// 清理崩溃/异常残留的 RTP 区域（mtpsrtp_ 前缀, 覆盖所有维度的 pending + active; 插件首个 tick 调用一次）
// 原理: /tickingarea 创建的区域是持久化的（存 LevelStorage, 重启会被引擎预加载）,
// 若上次运行中途崩溃没能移除, 会在重启后继续加载区块白吃性能 —— 必须兜底清除。
void purgeStaleRtpAreas(Level& level) {
    int removed = 0;
    try {
        auto& mgr     = level.getTickingAreasMgr();
        auto& storage = level.getLevelStorage();
        for (auto& [dim, list] : *mgr.mActiveAreas) {
            if (!list) continue;
            std::vector<std::shared_ptr<ITickingArea>> toRemove;
            for (auto& area : *list->mTickingAreas) {
                if (!area) continue;
                try {
                    if (area->isStandalone() && area->getName().rfind(RTP_AREA_PREFIX, 0) == 0) {
                        toRemove.push_back(area);
                    }
                } catch (std::exception const& e) {
                    RTP_DBG("[RTP][清理] 读取区域名失败: {}", e.what());
                } catch (...) {
                    RTP_DBG("[RTP][清理] 读取区域名失败: 未知异常");
                }
            }
            if (!toRemove.empty()) {
                list->removeAreas(toRemove, storage);
                removed += (int)toRemove.size();
            }
        }
        for (auto& [dim, vec] : *mgr.mPendingAreas) {
            for (auto it = vec.begin(); it != vec.end();) {
                bool mine = false;
                try { mine = it->mName->rfind(RTP_AREA_PREFIX, 0) == 0; } catch (...) {}
                if (mine) {
                    TickingAreasManager::_deletePendingArea(storage, *it);
                    it = vec.erase(it);
                    removed++;
                } else {
                    ++it;
                }
            }
        }
    } catch (...) {}
    if (removed > 0) {
        rtpLogger().info("[RTP] 已清理 {} 个上次运行残留的常加载区域", removed);
    }
}

} // namespace mtps
