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

// 请求处理区块（每 tick 都会被调, 所以不打日志）
bool requestChunkLoad(int dimid, int blockX, int blockZ) {
    auto level = ll::service::getLevel();
    if (!level) return false;
    auto dim = level->getDimension((::DimensionType)dimid).lock();
    if (!dim) return false;

    auto const cp     = ::ChunkPos(blockX >> 4, blockZ >> 4);
    auto&      source = (*dim).getChunkSource();
    if (source.getExistingChunk(cp)) return true;
    return source.createNewChunk(cp, ::ChunkSource::LoadMode::Deferred, /*readOnly*/ false) != nullptr;
}

//加载区域
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

    // 主动把区域内的区块加载
    return level.getTickingAreasMgr()._addArea(
        (::DimensionType)dimid, name, bounds, /*isCircle*/ true,
        TickingAreasManager::AreaLimitCheck::None, /*isPersistent*/ false,
        ::TickingAreaLoadMode::Preload, level.getLevelStorage());
}

// debug测加载速度
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

// debug检查是否处于加载阶段
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

// 移除随机传送的加载区域
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
// 兜底清除。
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
