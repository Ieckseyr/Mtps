#include "RandomTeleportInternal.h"

#include "Config.h"

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/Bedrock.h>

#include <mc/world/level/Level.h>
#include <mc/world/level/ChunkPos.h>
#include <mc/world/level/chunk/ChunkSource.h>
#include <mc/world/level/chunk/LevelChunk.h>
#include <mc/world/level/dimension/Dimension.h>

// 旧版本（用 /tickingarea 生成)残留区域的清理
#include <mc/world/level/ticking/ITickingArea.h>
#include <mc/world/level/ticking/PendingArea.h>
#include <mc/world/level/ticking/TickingAreaDescription.h>
#include <mc/world/level/ticking/TickingAreaList.h>
#include <mc/world/level/ticking/TickingAreasManager.h>

#include <memory>
#include <string>

namespace mtps {

// ── 区块生成/加载 ──────────────────────────────────────────────────────────
// 让引擎把一个区块弄到内存里, 两步（顺序不能反）:
//   1) getExistingChunk —— 已经在内存里就直接用;
//   2) createNewChunk  —— 内存里没有: 磁盘有就从盘载入, 从来没有过就让引擎生成。
// getOrLoadChunk 只能处理"已存在"的区块, 对从未生成过的会返回空（所以不能拿它当生成入口）。
//
// 另注: 也不能用「自建 ChunkViewSource + move」代理 —— 视野只是父源的一个过滤视图, 服务端
// 只驱动它自己登记过的那些视野（玩家的）, 插件自建的视野 move 之后什么都没发生（实测区块
// 状态一直停在 Unloaded）。
bool chunkLoadRequest(int dimid, int blockX, int blockZ) {
    auto level = ll::service::getLevel();
    if (!level) return false;
    auto dim = level->getDimension((::DimensionType)dimid).lock();
    if (!dim) return false;

    auto const chunkPos = ::ChunkPos(blockX >> 4, blockZ >> 4);
    auto&      source   = (*dim).getChunkSource();

    // 世界外的坐标引擎不会生成, 直接拒绝（免得白等一轮超时）
    if (!source.isWithinWorldLimit(chunkPos)) {
        RTP_DBG("[RTP][加载] 区块 ({}, {}) 超出世界边界, 拒绝", chunkPos.x, chunkPos.z);
        return false;
    }
    if (source.getExistingChunk(chunkPos)) return true;   // 已在内存, 无需处理

    auto chunk = source.createNewChunk(chunkPos, ::ChunkSource::LoadMode::None, /*readOnly*/ false);
    RTP_DBG("[RTP][加载] 区块 ({}, {}) dim={} → {}", chunkPos.x, chunkPos.z, dimid,
            chunk ? "已交给引擎（载入或生成）" : "失败");
    return chunk != nullptr;
}

// ── 旧版本残留清理 ─────────────────────────────────────────────────────────
// 早期实现用 /tickingarea 生成区块, 那些区域是**持久化**的（存 LevelStorage, 重启会被引擎
// 预加载）。已改用 getOrLoadChunk, 但需要把老版本可能残留的区域清掉, 否则会一直白吃性能。
// 只清理前缀匹配的区域, 首个 tick 调用一次。
void purgeLegacyTickingAreas(Level& level) {
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
                    if (area->isStandalone() && area->getName().rfind(RTP_LEGACY_AREA_PREFIX, 0) == 0) {
                        toRemove.push_back(area);
                    }
                } catch (...) {}
            }
            if (!toRemove.empty()) {
                list->removeAreas(toRemove, storage);
                removed += (int)toRemove.size();
            }
        }
        for (auto& [dim, vec] : *mgr.mPendingAreas) {
            for (auto it = vec.begin(); it != vec.end();) {
                bool mine = false;
                try { mine = it->mName->rfind(RTP_LEGACY_AREA_PREFIX, 0) == 0; } catch (...) {}
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
        rtpLogger().info("[RTP] 已清理 {} 个旧版本残留的常加载区域", removed);
    }
}

} // namespace mtps
