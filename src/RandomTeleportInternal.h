#pragma once
// RandomTeleport 内部共享声明: 会话状态机(RandomTeleport.cpp)、地表扫描(SurfaceScan.cpp)、
// 常加载区域(TickingAreaUtil.cpp) 三个文件共用的常量/类型/辅助函数, 免得各自复制一份。
#include "BedrockLevelReader.h"
#include "RandomTeleport.h"

#include <ll/api/io/Logger.h>
#include <mc/world/level/chunk/ChunkState.h>

#include <climits>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

class BlockSource;
class ITickingArea;
class Dimension;
class Level;
class Player;
enum class AddTickingAreaStatus : int;   // TickingAreasManager::_addArea 的返回值

namespace mtps {

// 调度参数
inline constexpr int RTP_CHUNK_WAIT_TICKS      = 200;   // 生成路径: 单落点区块等待上限（tick）
inline constexpr int RTP_SESSION_TIMEOUT_TICKS = 1200;  // 会话总超时（tick）
inline constexpr int RTP_STEPS_PER_TICK        = 8;     // 单会话每 tick 最多连续推进步数
// 等区块就绪时挂的常加载区域半径（区块）。引擎是按块生成/加载的, 区域越大中心越晚轮到
// （r=4 要处理 81 块 ≈ 50 tick, r=2 只 25 块 ≈ 13 tick）, 而且一次性往生成队列塞 81 块
// 也会把 MSPT 顶起来, 所以等待期和宽限期都用小区域。
inline constexpr int RTP_WAIT_AREA_RADIUS_CHUNKS = 2;
// 落点用的宽限区域半径。与等待半径相同 => 常见情况根本不用另挂区域（落点已在等待区域内）,
// 少一次"撤旧挂新"和几十块的额外生成; 玩家视野本来就会把周围拉起来。
inline constexpr int RTP_GRACE_AREA_RADIUS_CHUNKS = 2;
inline constexpr int RTP_EXPAND_MAX_RING       = 32;    // 扩圈上限（区块）
// 每个会话每 tick 的"内存扫区块"配额（硬上限, 跨步累计）。内存扫一块 ≈1ms（实测: 你日志里
// 一圈 42 块 ~44ms）, 所以这个数就是每 tick 给随机传送的主线程毫秒预算。
inline constexpr int RTP_SCAN_CHUNKS_PER_TICK  = 2;
// 每个会话每 tick 的"存档直读区块"配额（读盘 + 解压, 比内存扫更贵, 单独限一条）
inline constexpr int RTP_IO_CHUNKS_PER_TICK    = 2;
inline constexpr int RTP_TICK_BUDGET_MS        = 2;     // 每 tick 总时间预算（毫秒, 按会话平分）
inline constexpr int RTP_TICK_WARN_MS          = 5;     // 单 tick 超过这个耗时就在 debug 日志里报一行
inline constexpr int RTP_TABLE_CHUNKS_PER_TICK = 512;   // 每 tick 最多走落点表（零 IO）的区块数
inline constexpr int RTP_AREA_GRACE_TICKS      = 200;   // 传送成功后区域的宽限保留时长（tick）
inline constexpr int RTP_SPAWN_WAIT_TICKS      = 200;   // 出生流程未走完时的等待上限（tick）
inline constexpr int RTP_LANDING_HOLD_TICKS    = 40;    // 落点周围还没加载完时, 最多再等多少拍
inline constexpr int RTP_PUBLISH_RECHECK_TICKS = 2;     // 传送到位后核对客户端发布区域的间隔（tick）
inline constexpr int RTP_PUBLISH_MAX_RETRY     = 3;     // 发布区域核对最多重试几次
inline constexpr char RTP_AREA_PREFIX[]        = "mtpsrtp_";  // 区域名前缀（清理残留用）
// 落点表缓存命中时不必重算; 没命中则推迟到开服稳定后再重建（别和开服抢磁盘）
inline constexpr int RTP_LANDING_DEFER_SECONDS = 45;

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

// 判定用的是哪个数据源（扩圈汇总日志按来源计数, 便于区分"大洋"和"没生成"）
enum class ChunkSource { None, Memory, Table, Archive };

// 三源逐级判定一块: 内存 -> 落点表 -> 存档直读
// cheapOut 表示这次是"零 IO 零分配"（走的落点表）, 调用方据此决定是否计入 tick 预算
ChunkVerdict resolveChunk(
    Dimension& dim, int dimid, int cx, int cz,
    YRange yRange, int scanStartY,
    std::unordered_set<std::string> const& dangerSet,
    std::unordered_set<std::string> const& dangerShortSet,
    SafePos& out, std::string& reason, bool& cheapOut,
    ChunkSource* srcOut = nullptr
);

// 常加载区域（命令封装）
std::string makeAreaName(std::string const& playerName);
// 登记常加载区域（引擎 API, 不落盘; 见 TickingAreaUtil.cpp）
AddTickingAreaStatus addRtpArea(Level& level, int dimid, std::string const& name,
                                int blockX, int blockZ, int radiusChunks);
bool        requestChunkLoad(int dimid, int blockX, int blockZ);   // 直接请求引擎处理该区块（Deferred）
void        removeRtpArea(Level& level, int dimid, std::string const& name);
bool         areaStillPending(Level& level, int dimid, std::string const& name);   // 诊断用
ITickingArea* findRtpArea(Level& level, int dimid, std::string const& name);       // 诊断用
void        purgeStaleRtpAreas(Level& level);

} // namespace mtps
