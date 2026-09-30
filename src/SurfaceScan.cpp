#include "ArchiveScanner.h"
#include "RandomTeleportInternal.h"
#include "SurfaceRules.h"

#include <ll/api/mod/NativeMod.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>

#include <mc/server/ServerLevel.h>
#include <mc/world/level/BlockSource.h>
#include <mc/world/level/BlockPos.h>
#include <mc/world/level/block/Block.h>
#include <mc/world/level/ChunkPos.h>
#include <mc/world/level/chunk/LevelChunk.h>
#include <mc/world/level/chunk/ChunkSource.h>
#include <mc/world/level/dimension/Dimension.h>

namespace mtps {

YRange getDimYRange(int dim) {
    switch (dim) {
        case 0: return {-64, 318};  // 主世界
        case 1: return {0, 125};    // 下界
        case 2: return {0, 254};    // 末地
        default: return {-64, 318};
    }
}
int getScanStartY(int dim) {
    switch (dim) {
        case 0: return 200;
        case 1: return 120;
        case 2: return 200;
        default: return 200;
    }
}

// 水方块（浅水判定; 深水不要）。只有两种, 直接比字符串比查哈希表便宜, 也省一张表。
static bool isWaterName(std::string const& n) { return n == "water" || n == "flowing_water"; }

// 区块就绪探测（零成本查询, 不触发任何加载）
// 必须等到 ChunkState::Loaded（终态）才能扫描 —— 之前的阶段子区块还是占位空数据。
ChunkState chunkStateAt(Dimension& dim, int blockX, int blockZ) {
    auto lc = dim.getChunkSource().getExistingChunk(ChunkPos(blockX >> 4, blockZ >> 4));
    if (!lc) return ChunkState::Unloaded;
    return lc->mLoadState->load();
}
bool isChunkReady(Dimension& dim, int blockX, int blockZ) {
    return chunkStateAt(dim, blockX, blockZ) >= ChunkState::Loaded;
}

// 地表安全列扫描（BlockSource 版; JS findSurfaceSafePosition 忠实移植）

// hintY: 上一列的地表高度（RTP_NO_HINT 表示没有）。相邻列高度通常只差几格, 先用它附近试一次。
// 提示只用于加速: 试不中就走完整流程。命中的是"符合安全条件的站立位", 与完整流程的落点
// 可能不同（提示下方有成层地形时会取较低的那个）, 两者都安全。
ScanResult findSurfaceSafe(
    BlockSource& bs, int blockX, int blockZ, int dim,
    YRange yRange, int scanStartY,
    std::unordered_set<std::string> const& dangerSet,
    int hintY
) {
    ScanResult res;
    double tpX = blockX + 0.5;
    double tpZ = blockZ + 0.5;
    auto blockAt = [&](int y) -> Block const& { return bs.getBlock(BlockPos(blockX, y, blockZ)); };

    // 树叶对找地表是透明的（见 SurfaceRules.h）：穿过它继续往下，落点取它下面的地面，
    // 否则森林里的落点全是树冠顶。读方块比取名字贵，所以下面每处只查一次、判完空气再判树叶。
    auto solidNonLeafAt = [&](int y) -> bool {
        Block const& b = blockAt(y);
        if (b.isAir()) return false;
        return !isLeafBlockName(b.getTypeName());
    };

    // 提示位置上方必须是空气才敢从它开始往下找, 否则可能在悬崖/洞穴内部,
    // 会落到底下而不是地表。试不中直接走完整流程, 所以不会改变结果。
    if (hintY != RTP_NO_HINT && hintY > yRange.minY) {
        int const from = std::min(hintY + 4, scanStartY);
        if (from >= yRange.minY && blockAt(from).isAir()) {
            int const to = std::max(hintY - 10, yRange.minY);
            for (int y = from; y >= to; y--) {
                Block const& bot = blockAt(y);
                if (bot.isAir()) continue;                 // 先判空气: 多数格子都是空气, 省掉取名字
                std::string const botName = bot.getTypeName();
                if (isWaterName(botName) || dangerSet.count(botName)) continue;
                if (isLeafBlockName(botName)) continue;    // 树冠: 继续往下找地面
                if (blockAt(y + 1).isAir() && blockAt(y + 2).isAir()) {
                    res.found = true;
                    res.pos   = {tpX, (double)(y + 1), tpZ, dim};
                    return res;
                }
            }
        }
    }

    // 第一步：大步（8 格）往下找地表（树叶不算地表）
    int coarseSolidY = -1;
    for (int y = scanStartY; y >= yRange.minY; y -= 8) {
        if (solidNonLeafAt(y)) { coarseSolidY = y; break; }   // 一次 getBlock 判完空气与树叶
    }
    if (coarseSolidY == -1) {
        res.reason    = "整列空气(虚空)";
        res.openWater = true;
        return res;
    }

    // 第二步：小步找准地表精确位置
    int surfaceY = coarseSolidY;
    int fineTop  = std::min(coarseSolidY + 7, scanStartY);
    for (int y = fineTop; y > coarseSolidY; y--) {
        if (solidNonLeafAt(y)) { surfaceY = y; break; }
    }

    res.surfaceY      = surfaceY;   // 地表高度（可能后面判为不可用, 但仍可当下一列的提示）
    std::string surfaceName = blockAt(surfaceY).getTypeName();

    // 地表是水：逐格判定水深（身高 1.8 格: 水深 2 格头就浸水里）
    if (isWaterName(surfaceName)) {
        int depth = 1;
        int floorY = surfaceY - 1;
        std::string floorName = blockAt(floorY).getTypeName();
        while (isWaterName(floorName) && depth < 2) {
            depth++; floorY--;
            floorName = blockAt(floorY).getTypeName();
        }
        if (depth > 1) {
            res.reason    = "深水";
            res.openWater = true;
            return res;
        }
        Block const& floor = blockAt(floorY);
        if (floor.isAir() || dangerSet.count(floor.getTypeName())) { res.reason = "水底站不住"; return res; }
        if (!blockAt(surfaceY + 1).isAir()) { res.reason = "水上头格被占"; return res; }
        if (!blockAt(surfaceY + 2).isAir()) { res.reason = "水上头格被占"; return res; }
        res.found = true;
        res.pos   = {tpX, (double)(floorY + 1), tpZ, dim}; // 站水底，水刚没过脚
        return res;
    }

    // 岩浆/仙人掌这些真危险的，整列不要
    if (dangerSet.count(surfaceName)) { res.reason = "地表危险方块:" + surfaceName; return res; }

    // 第三步：从地表往下找站位（最多 32 格）
    int minCheckY = std::max(surfaceY - 32, yRange.minY);
    for (int y = surfaceY; y >= minCheckY; y--) {
        Block const& bot = blockAt(y);
        if (bot.isAir()) continue;                 // 先判空气, 免得白取一次方块名
        std::string const botName = bot.getTypeName();
        if (isWaterName(botName)) continue;        // 水块：向下找地板
        if (dangerSet.count(botName)) continue;    // 危险方块：向下
        if (isLeafBlockName(botName)) continue;    // 树冠：向下找地面（不在树顶落脚）
        if (blockAt(y + 1).isAir() && blockAt(y + 2).isAir()) { // 上方两格空气（身高 2 格）
            res.found = true;
            res.pos   = {tpX, (double)(y + 1), tpZ, dim};
            return res;
        }
    }
    res.reason = "地表32格内无站位"; // 32 格内没有安全位置
    return res;
}

// 存档 ChunkSurface 安全列判定（数据源②③共用）: 水深 0 → 站地表; 水深 1 → 站水底上方
// （脚浸水头露出, 与 JS 浅水判定一致）; 水深 >1 或地表是危险方块 → 不可用。
bool findSafeInSurface(
    BedrockLevelReader::ChunkSurface const& sfc, int cx, int cz, int dim,
    std::unordered_set<std::string> const& dangerShort,
    SafePos& out, std::string& reason
) {
    int bestLx = -1, bestLz = -1, bestDist = INT_MAX, bestY = 0;
    int  waterCols = 0, emptyCols = 0, dangerCols = 0;
    for (int lx = 0; lx < 16; lx++) {
        for (int lz = 0; lz < 16; lz++) {
            int i = lx * 16 + lz;
            if (sfc.height[i] == INT16_MIN) { emptyCols++; continue; }  // 虚空/无数据列
            std::string const& name = sfc.block[i];
            if (name.empty()) { emptyCols++; continue; }
            if (dangerShort.count(name)) { dangerCols++; continue; }   // 危险地表（岩浆等）
            if (sfc.waterDepth[i] > 1) { waterCols++; continue; }      // 深水
            int dist = std::abs(lx - 8) + std::abs(lz - 8);
            if (dist < bestDist) {
                bestDist = dist;
                bestLx = lx; bestLz = lz;
                bestY  = (int)sfc.height[i] + 1;
            }
        }
    }
    if (bestLx < 0) {
        reason = "chunk无安全列(虚空" + std::to_string(emptyCols)
               + "/深水" + std::to_string(waterCols)
               + "/危险" + std::to_string(dangerCols) + ")";
        return false;
    }
    out = {cx * 16 + bestLx + 0.5, (double)bestY, cz * 16 + bestLz + 0.5, dim};
    return true;
}


// 中心向外的列扫描顺序（按到 chunk 中心的切比雪夫距离分圈）。行序扫的话命中列可能离目标很远，
// 按圈扫先看中心附近（与预计算表"取离 chunk 中心最近的可用列"同一口径），
// 而且陆地上通常最内几圈就命中，整块的工作量小得多。
struct LxLz { uint8_t lx, lz; };
std::vector<LxLz> const& centerOutOrder() {
    static std::vector<LxLz> const v = [] {
        std::vector<LxLz> o;
        o.reserve(256);
        for (int r = 0; r <= 8; r++) {
            for (int lx = 0; lx < 16; lx++) {
                for (int lz = 0; lz < 16; lz++) {
                    if (std::max(std::abs(lx - 8), std::abs(lz - 8)) == r) {
                        o.push_back({(uint8_t)lx, (uint8_t)lz});
                    }
                }
            }
        }
        return o;
    }();
    return v;
}
// 先扫的内圈半径（列）。2 圈 = 25 列，覆盖 5x5 —— 陆地几乎必然命中。
constexpr int kInnerRings = 2;

// 数据源①: 内存缓存（Loaded 终态, 数据完整且最新, 零 IO）
bool scanChunkMemory(
    Dimension& dim, int dimid, int cx, int cz,
    YRange yRange, int scanStartY,
    std::unordered_set<std::string> const& dangerSet,
    SafePos& out
) {
    BlockSource& bs = dim.getBlockSourceFromMainChunkSource();

    // 先扫中心 5x5：陆地几乎必然命中，常见情况的扫描量从最多 256 列降到 25 列。
    // 没命中才走下面的水域快筛 + 全列扫描（那两条留给大洋/异常地形）。
    int hintY = RTP_NO_HINT;   // 上一列的地表高度, 喂给下一列省掉粗扫
    for (auto const& c : centerOutOrder()) {
        if (std::max(std::abs((int)c.lx - 8), std::abs((int)c.lz - 8)) > kInnerRings) break;
        auto res = findSurfaceSafe(bs, cx * 16 + c.lx, cz * 16 + c.lz, dimid, yRange, scanStartY,
                                   dangerSet, hintY);
        if (res.found) { out = res.pos; return true; }
        if (res.surfaceY != RTP_NO_HINT) hintY = res.surfaceY;   // 没站位也有地表高度可用作提示
    }

    // 纯水域快筛: 隔一格取 8x8 = 64 个样本列, 全是深水/虚空就认为整块是大洋, 不必扫 256 列。
    // 这一步是有损的: 采样点没覆盖到的孤立 1x1 小柱仍可能被漏掉, 后果是那次传送换个点重随,
    // 不会把玩家送到危险位置。步长 2 能覆盖 2x2 及以上的礁石, 再密就接近全扫、没有意义。
    {
        int hit = 0, total = 0;
        for (int lx = 0; lx < 16; lx += 2) {
            for (int lz = 0; lz < 16; lz += 2) {
                total++;
                auto res = findSurfaceSafe(bs, cx * 16 + lx, cz * 16 + lz, dimid,
                                           yRange, scanStartY, dangerSet);
                if (res.openWater) hit++;
            }
        }
        if (total > 0 && hit == total) {
            RTP_DBG("[RTP][扫描] chunk({},{}) 采样全是深水/虚空, 跳过整块", cx, cz);
            return false;
        }
    }

    for (auto const& c : centerOutOrder()) {
        auto res = findSurfaceSafe(bs, cx * 16 + c.lx, cz * 16 + c.lz, dimid,
                                   yRange, scanStartY, dangerSet, hintY);
        if (res.found) { out = res.pos; return true; }
        // 没找到时也能拿到"这列的地表高度"当下一列的提示（原来那句更新写在 return 之后,
        // 永远执行不到 —— 提示机制其实一直没生效）。提示只加速、不改变结果。
        if (res.surfaceY != RTP_NO_HINT) hintY = res.surfaceY;
    }
    return false;
}

// cheapOut: 本次判定是否"零 IO 零分配"（走落点表）—— 供调用方决定是否计入 tick 预算
// srcOut: 本次用的是哪个数据源（诊断用, 可空）
ChunkVerdict resolveChunk(
    Dimension& dim, int dimid, int cx, int cz,
    YRange yRange, int scanStartY,
    std::unordered_set<std::string> const& dangerSet,
    std::unordered_set<std::string> const& dangerShortSet,
    SafePos& out, std::string& reason, bool& cheapOut,
    ChunkSource* srcOut
) {
    cheapOut = false;
    if (srcOut) *srcOut = ChunkSource::None;

    // ① 内存缓存优先: 已加载 chunk 的内存数据最新（玩家刚改动的方块存档层看不到）
    if (isChunkReady(dim, cx << 4, cz << 4)) {
        if (srcOut) *srcOut = ChunkSource::Memory;
        if (scanChunkMemory(dim, dimid, cx, cz, yRange, scanStartY, dangerSet, out)) return ChunkVerdict::Safe;
        reason = "内存扫:整chunk无安全列";
        return ChunkVerdict::Unsafe;
    }

    // ② 落点预计算表: 判定退化为一次内存二分（RTP 扩圈的主要路径）
    BedrockLevelReader::Landing ld{};
    if (ArchiveScanner::getInstance().lookupLanding(cx, cz, dimid, ld)) {
        cheapOut = true;
        if (srcOut) *srcOut = ChunkSource::Table;
        if (ld.state == BedrockLevelReader::LAND_SAFE) {
            out = {cx * 16 + ld.lx + 0.5, (double)ld.y, cz * 16 + ld.lz + 0.5, dimid};
            return ChunkVerdict::Safe;
        }
        reason = "落点表:整chunk无安全列";
        return ChunkVerdict::Unsafe;
    }
    // 表已就绪时, 查不到即"存档确实没有该 chunk"（表覆盖索引里的全部 chunk）。
    // 这也是一次内存二分, 同样算廉价 —— 否则未生成的荒野会把 tick 预算吃光,
    // 扩圈一圈要扫几十 tick（扩圈的主要成本应该只是"真去内存扫"的那些块）。
    if (ArchiveScanner::getInstance().landingsReady()) {
        cheapOut = true;
        reason = "存档无此chunk";
        return ChunkVerdict::NoData;
    }

    // ③ 存档直读兜底: 表还在后台算（启动初期）时, 保持原同步直读行为
    if (ArchiveScanner::getInstance().ready()) {
        if (srcOut) *srcOut = ChunkSource::Archive;
        auto sfc = ArchiveScanner::getInstance().scanChunk(cx, cz, dimid);
        bool hasAny = false;
        for (int i = 0; i < 256; i++) {
            if (!sfc.block[i].empty() || sfc.height[i] != INT16_MIN) { hasAny = true; break; }
        }
        if (hasAny) {
            if (findSafeInSurface(sfc, cx, cz, dimid, dangerShortSet, out, reason)) return ChunkVerdict::Safe;
            return ChunkVerdict::Unsafe;
        }
        reason = "存档无此chunk";
        return ChunkVerdict::NoData;
    }
    reason = "无数据源(内存未加载+存档未就绪)";
    return ChunkVerdict::NoData;
}

} // namespace mtps
