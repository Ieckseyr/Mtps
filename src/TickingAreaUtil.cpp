#include "RandomTeleportInternal.h"
#include "TpUtil.h"

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

// 以 Owner 权限在指定维度执行原版命令（主线程调用; suppressOutput=true 静默）
bool rtpRunCommand(Level& level, std::string const& cmd, int dimid) {
    (void)level;   // 统一走 TpUtil 的命令执行（内部自己取 Level）
    return runServerCommand(dimid, cmd);
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
