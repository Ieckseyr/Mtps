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

// ── 区块加载 ───────────────────────────────────────────────────────────────
// 让引擎加载/生成指定区块: ChunkSource::getOrLoadChunk —— 磁盘里有就载入内存, 没有就排队生成。
//
// 注: 不能用「自建 ChunkViewSource + move」代理: 视野只是父源的一个过滤视图, 服务端只驱动
// 它自己登记过的那些视野, 插件自建的视野 move 之后没有任何东西被加载（实测区块状态一直
// Unloaded）。getOrLoadChunk 才是引擎自己的加载入口, 与玩家视野走同一套加载/生成流水线。
bool chunkLoadRequest(int dimid, int blockX, int blockZ) {
    auto level = ll::service::getLevel();
    if (!level) return false;
    auto dim = level->getDimension((::DimensionType)dimid).lock();
    if (!dim) return false;

    auto  chunkPos = ::ChunkPos(blockX >> 4, blockZ >> 4);
    auto& source   = (*dim).getChunkSource();
    // readOnly=false: 允许创建/生成（readOnly=true 只会拿已存在的）
    auto  chunk    = source.getOrLoadChunk(chunkPos, ::ChunkSource::LoadMode::None, false);
    RTP_DBG("[RTP][加载] 请求区块 ({}, {}) dim={} → {}", blockX >> 4, blockZ >> 4, dimid,
            chunk ? "已受理" : "失败");
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
