#include "RandomTeleportInternal.h"

#include "Config.h"
#include "TpUtil.h"

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/Bedrock.h>

#include <mc/world/level/BlockPos.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/chunk/ChunkSource.h>
#include <mc/world/level/chunk/ChunkSourceViewGenerateMode.h>
#include <mc/world/level/chunk/ChunkViewSource.h>
#include <mc/world/level/chunk/LevelChunk.h>

// 旧版本（用 /tickingarea 生成)残留区域的清理
#include <mc/world/level/ticking/ITickingArea.h>
#include <mc/world/level/ticking/PendingArea.h>
#include <mc/world/level/ticking/TickingAreaDescription.h>
#include <mc/world/level/ticking/TickingAreaList.h>
#include <mc/world/level/ticking/TickingAreasManager.h>

#include <memory>
#include <string>

namespace mtps {

// ── 区块视野（ChunkViewSource）──────────────────────────────────────────────
// 引擎原生的"无玩家也加载/生成区块"入口: 覆盖范围内的区块会被加载, GenerateAll 连新生地形
// 一起生成。换点只需 move（不用重建）, 释放后区块交还引擎按常规规则卸载（玩家在附近时
// 由玩家自己的视野接管, 不会掉）。
// 持有即生效: 引擎在后续 tick 里处理加载请求, 所以调用方 move 完要等 chunkStateAt 就绪。
struct ChunkView {
    std::unique_ptr<::ChunkViewSource> source;
};

std::shared_ptr<ChunkView> chunkViewCreate(int dimid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto dim = level->getDimension((::DimensionType)dimid).lock();
    if (!dim) return nullptr;

    auto view    = std::make_shared<ChunkView>();
    view->source = std::make_unique<::ChunkViewSource>((*dim).getChunkSource(), ::ChunkSource::LoadMode::None);
    if (!view->source) return nullptr;
    RTP_DBG("[RTP][视野] 已建立 dim={} 的区块视野", dimid);
    return view;
}

// 把视野移到以 (blockX, blockZ) 为中心、radiusBlocks 为半径的方形区域
// （方形比圆覆盖更多角落, 扩圈找到的落点更不容易落在区域外; 覆盖判定用欧氏距离, 偏保守）
bool chunkViewMove(ChunkView& view, int blockX, int blockZ, int radiusBlocks) {
    if (!view.source) return false;
    view.source->move(
        ::BlockPos(blockX, 64, blockZ),
        radiusBlocks,
        /*isCircle*/ false,
        ::ChunkSourceViewGenerateMode::GenerateAll,
        [](::gsl::span<::std::shared_ptr<::LevelChunk>>) {},   // 加载进度不关心: 就绪与否轮询 chunkStateAt
        nullptr                                                // serverBuildRatio: 用引擎默认
    );
    RTP_DBG("[RTP][视野] 请求加载: 中心 ({}, {}) 半径 {} 格", blockX, blockZ, radiusBlocks);
    return true;
}

// ── 旧版本残留清理 ─────────────────────────────────────────────────────────
// 早期实现用 /tickingarea 生成区块, 那些区域是**持久化**的（存 LevelStorage, 重启会被引擎
// 预加载）。已改用区块视野, 但需要把老版本可能残留的区域清掉, 否则会一直白吃性能。
// 只清理前缀匹配的区域, 首次 tick 调用一次。
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
