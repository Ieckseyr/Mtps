#pragma once
// RandomTeleport 内部共享声明: 会话状态机(RandomTeleport.cpp)、地表扫描(SurfaceScan.cpp)、
// 区块加载(ChunkLoadUtil.cpp) 三个文件共用的常量/类型/辅助函数, 免得各自复制一份。
#include "BedrockLevelReader.h"
#include "RandomTeleport.h"

#include <ll/api/io/Logger.h>
#include <mc/world/level/chunk/ChunkState.h>

#include <climits>
#include <memory>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

class BlockSource;
class Dimension;
class Level;
class Player;

namespace mtps {

// 调度参数
inline constexpr int RTP_CHUNK_WAIT_TICKS      = 200;   // 生成路径: 单落点区块等待上限（tick）
inline constexpr int RTP_SESSION_TIMEOUT_TICKS = 1200;  // 会话总超时（tick）
inline constexpr int RTP_STEPS_PER_TICK        = 8;     // 单会话每 tick 最多连续推进步数
inline constexpr int RTP_LOAD_RADIUS_CHUNKS    = 4;     // 主动请求生成的扩圈范围（区块）
inline constexpr int RTP_EXPAND_MAX_RING       = 24;    // 扩圈上限（区块）
inline constexpr int RTP_SCAN_CHUNKS_PER_TICK  = 6;     // 每 tick 最多"内存扫/存档直读"的区块数
inline constexpr int RTP_TICK_BUDGET_MS        = 4;     // 每 tick 总时间预算（毫秒, 按会话平分）
inline constexpr int RTP_TABLE_CHUNKS_PER_TICK = 256;   // 每 tick 最多走落点表（零 IO）的区块数
inline constexpr int RTP_VIEW_GRACE_TICKS       = 200;   // 传送成功后落点区块的宽限保持时长（tick）
inline constexpr int RTP_LOAD_PUMP_TICKS       = 20;    // 同一区块两次加载请求的最小间隔（tick）
inline constexpr int RTP_SPAWN_WAIT_TICKS      = 200;   // 出生流程未走完时的等待上限（tick）
inline constexpr char RTP_LEGACY_AREA_PREFIX[] = "mtpsrtp_";  // 旧版 /tickingarea 区域名前缀（仅清理残留）

// 日志（debug 开关在 config 的 randomTeleport.debug）
ll::io::Logger& rtpLogger();
bool            rtpDebugEnabled();

#define RTP_DBG(...) do { if (mtps::rtpDebugEnabled()) mtps::rtpLogger().info(__VA_ARGS__); } while (0)

// 维度的 Y 范围与扫描起点
struct YRange { int minY, maxY; };
YRange getDimYRange(int dim);
int    getScanStartY(int dim);

// 区块状态
char const* chunkStateName(ChunkState st);
ChunkState  chunkStateAt(Dimension& dim, int blockX, int blockZ);
bool        isChunkReady(Dimension& dim, int blockX, int blockZ);

// 地表扫描结果
struct ScanResult {
    bool        found{false};
    bool        openWater{false};   // 深水或整列虚空, 纯海域快筛用
    SafePos     pos{};
    std::string reason;
};

// hintY 的"没有提示"哨兵。不能用 0: 主世界 y=0 是合法高度, 会被当成有效提示,
// 于是从 y=0 附近往下找, 可能落到峡谷/洞穴里的台子上。
inline constexpr int RTP_NO_HINT = INT_MIN;

// 逐列找安全落点（内存路径）。hintY 是上一列的地表高度, 用来省掉粗扫。
// 提示只是加速: 试不中就走完整流程。命中时给出的是"符合安全条件的站立位",
// 不一定与完整流程的落点完全相同（例如提示下方有成层地形时会取较低的那个）。
ScanResult findSurfaceSafe(
    BlockSource& bs, int blockX, int blockZ, int dim,
    YRange yRange, int scanStartY,
    std::unordered_set<std::string> const& dangerSet,
    int hintY = RTP_NO_HINT
);

// 从存档表面数据里挑安全列（存档路径）
bool findSafeInSurface(
    BedrockLevelReader::ChunkSurface const& sfc, int cx, int cz, int dim,
    std::unordered_set<std::string> const& dangerShort,
    SafePos& out, std::string& reason
);

// 内存路径扫一整块
bool scanChunkMemory(
    Dimension& dim, int dimid, int cx, int cz,
    YRange yRange, int scanStartY,
    std::unordered_set<std::string> const& dangerSet,
    SafePos& out
);

// 单块判定结论
enum class ChunkVerdict { Safe, Unsafe, NoData };

// 三源逐级判定一块: 内存 -> 落点表 -> 存档直读
// cheapOut 表示这次是"零 IO 零分配"（走的落点表）, 调用方据此决定是否计入 tick 预算
ChunkVerdict resolveChunk(
    Dimension& dim, int dimid, int cx, int cz,
    YRange yRange, int scanStartY,
    std::unordered_set<std::string> const& dangerSet,
    std::unordered_set<std::string> const& dangerShortSet,
    SafePos& out, std::string& reason, bool& cheapOut
);

// 区块加载（ChunkSource::getOrLoadChunk 封装; 定义在 ChunkLoadUtil.cpp）
bool chunkLoadRequest(int dimid, int blockX, int blockZ);   // 让引擎载入/生成已存在的区块
bool chunkInWorldLimit(int dimid, int blockX, int blockZ);   // 是否在引擎允许生成的世界范围内
void purgeLegacyTickingAreas(Level& level);                 // 清理旧版 /tickingarea 残留区域

} // namespace mtps
