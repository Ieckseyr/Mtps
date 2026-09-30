// RandomTeleport.cpp - 随机传送（四级数据源 + 引擎 TickingArea 兜底版）
// 会话状态机: 随机选点 → 判定落点 chunk（内存 → 落点预计算表 → 存档直读 → tickingarea 生成）
// → 整 chunk 无安全列则按 chunk 粒度扩圈(1~24) → 全失败换点重来(最多 6 次) → 仍失败退款。
// 演进过程与各数据源的取舍见 README「随机传送怎么找安全落点」。
#include "RandomTeleportInternal.h"
#include "ArchiveScanner.h"
#include "BiomeSampler.h"
#include "Config.h"
#include "Economy.h"
#include "PoolWarmer.h"
#include "PreLandingPool.h"
#include "TpUtil.h"

#include <ll/api/service/Bedrock.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <mc/server/ServerLevel.h>
#include <mc/server/commands/CommandContext.h>
#include <mc/server/commands/CommandPermissionLevel.h>
#include <mc/server/commands/CurrentCmdVersion.h>
#include <mc/server/commands/MinecraftCommands.h>
#include <mc/server/commands/ServerCommandOrigin.h>
#include <mc/deps/core/utility/MCRESULT.h>
#include <mc/world/Minecraft.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/BlockSource.h>
#include <mc/world/level/BlockPos.h>
#include <mc/world/level/block/Block.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/level/chunk/ChunkSource.h>
#include <mc/world/level/chunk/LevelChunk.h>
#include <mc/world/level/chunk/ChunkState.h>
#include <mc/world/level/ChunkPos.h>
#include <mc/world/level/ticking/ITickingArea.h>
#include <mc/world/level/ticking/ITickingAreaView.h>
#include <mc/world/level/ticking/PendingArea.h>
#include <mc/world/level/ticking/TickingAreaDescription.h>
#include <mc/world/level/ticking/TickingAreaList.h>
#include <mc/world/level/ticking/TickingAreasManager.h>
#include <mc/deps/core/math/Vec3.h>
#include <mc/network/packet/SetTitlePacket.h>
#include <mc/network/packet/SetTitlePacketPayload.h>
#include <mc/server/NetworkChunkPublisher.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <random>
#include <algorithm>
#include <climits>
#include <unordered_set>
#include <chrono>
#include <ctime>
#include <cstdint>

// 26.40: SetTitlePacket 的 Payload 默认构造是 "prevent constructor by default" 模式,
// `SetTitlePacket pkt{}` 会实例化 PayloadPacket<>() 引用 SetTitlePacketPayload(), 需显式 = default
SetTitlePacketPayload::SetTitlePacketPayload() = default;

namespace mtps {

constexpr double kPi = 3.14159265358979323846;

// actionbar 提示
static void sendActionbar(Player& p, std::string const& text) {
    SetTitlePacket pkt{};
    pkt.mType      = SetTitlePacketPayload::TitleType::Actionbar;
    pkt.mTitleText = text;
    pkt.sendTo(p);
}

// 落点位置带 +0.5 偏移: 负数直接强转会向上取整, 日志里看起来与"落点区块"差一格。
// 统一取 floor 才是它真正落在的方块（排查"落点到底在哪"时不会再自相矛盾）。
inline int blockXOf(double v) { return (int)std::floor(v); }

// 每个玩家最近一次随机传送的落点（只用于"连续两次别落同一片"的间隔约束；重启即忘，
// 不影响任何正确性）。单线程（都在 tick 里）访问，无需加锁。
static std::unordered_map<std::string, std::pair<int, int>>& sLastLanding() {
    static std::unordered_map<std::string, std::pair<int, int>> m;
    return m;
}

// Debug 日志（config: 顶层 debug 或 randomTeleport.debug，两者任一开着都输出）
// 定义放这里, 声明在 RandomTeleportInternal.h（三个实现文件共用）
ll::io::Logger& rtpLogger() { return ll::mod::NativeMod::current()->getLogger(); }
bool            rtpDebugEnabled() {
    // 顶层 debug 是总开关（README 与 Config.h 都这么写）。以前这里只读 randomTeleport.debug,
    // 于是"把 debug 打开"的人看不到随机传送的任何过程 —— 两个开关任一为真即输出。
    auto& cfg = Config::getInstance();
    return cfg.debug() || cfg.randomDebug();
}
char const* chunkStateName(ChunkState st) {
    switch (st) {
        case ChunkState::Unloaded:                   return "Unloaded";
        case ChunkState::Generating:                 return "Generating";
        case ChunkState::Generated:                  return "Generated";
        case ChunkState::StructurePostProcessing:     return "StructPostProc";
        case ChunkState::StructurePostProcessed:    return "StructPostProcDone";
        case ChunkState::DecorationPostProcessing:    return "DecorPostProc";
        case ChunkState::DecorationPostProcessed:     return "DecorPostProcDone";
        case ChunkState::CheckingForReplacementData:  return "CheckReplace";
        case ChunkState::NeighborAwareUpgradeNeeded: return "NeighborUpgradeNeeded";
        case ChunkState::NeighborAwareUpgrading:     return "NeighborUpgrading";
        case ChunkState::NeedsLighting:              return "NeedsLighting";
        case ChunkState::Lighting:                    return "Lighting";
        case ChunkState::LightingFinished:           return "LightingDone";
        case ChunkState::Loaded:                     return "Loaded";
        default:                                     return "Invalid";
    }
}

RandomTeleport& RandomTeleport::getInstance() {
    static RandomTeleport instance;
    return instance;
}

// 常加载区域（TickingArea）: 引擎原生的"无玩家强制加载区块"机制
// 走 /tickingarea 命令的完整原生路径（Bounds 构造/激活/持久化全在引擎内）。

// 玩家名 + 全局序号 → 合法区域名（命令字符串参数, 只留字母数字下划线; 序号保证永不重名）
struct Offset { int dx, dz; };

// 一圈的周界偏移（r 圈周长约 8r）
static std::vector<Offset> buildRing(int r) {
    std::vector<Offset> v;
    v.reserve((size_t)8 * r);
    for (int dx = -r; dx <= r; dx++) v.push_back({dx, -r});
    for (int dz = -r + 1; dz <= r; dz++) v.push_back({r, dz});
    for (int dx = r - 1; dx >= -r; dx--) v.push_back({dx, r});
    for (int dz = r - 1; dz >= -r + 1; dz--) v.push_back({-r, dz});
    return v;
}

// 全部圈一次建好（静态初始化线程安全, 不必再判空）
static std::vector<Offset> const& expandRing(int r) {
    static std::vector<std::vector<Offset>> const cache = [] {
        std::vector<std::vector<Offset>> all(RTP_EXPAND_MAX_RING + 1);
        for (int rr = 1; rr <= RTP_EXPAND_MAX_RING; rr++) all[rr] = buildRing(rr);
        return all;
    }();
    return cache[r];
}

// 会话
struct RandomTeleport::Session {
    std::string playerName;  // 每 tick 按名查 Player（防离线悬空指针）
    int         dimid{0};
    std::string message;
    int         cost{0};

    double      originX{0}, originZ{0};
    int         radius{1000};
    bool        usePlayerOrigin{false};

    YRange      yRange;
    int         scanStartY{0};

    int         reRandomLeft{6};   // 见 start(): 由 config randomTeleport.maxAttempts 决定（有上限保护）
    bool        usedKnownFallback{false};  // 重随名额用尽后是否已试过"已知安全点"兜底
    std::vector<std::pair<int,int>> triedTargets;

    int         randomX{0}, randomZ{0};

    // 预落点池命中信息（未命中时 poolHit=false，一切照旧走降级链路）
    bool        poolHit{false};
    bool        poolProbed{false};   // 该点的 chunk 已经过判定（决定要不要回写失效）
    int         poolX{0}, poolZ{0};  // 池里的点本身（回写状态用；可能被抖动过）
    int         poolTier{0};         // kPlTierLoaded / kPlTierUnloaded

    // 落点缓存：等"周围区块加载完"的这段（最多 40 拍）里，每 tick 重扫同一个区块是纯浪费 ——
    // 一次内存全列扫描约 1ms，实测 12 拍就是 12ms 的额外 MSPT。缓存住上次扫到的落点，
    // 只在新落点/超过 10 拍时才真正重扫（地形在这几百毫秒里不会变）。
    bool    haveCachedPos{false};
    SafePos cachedPos{};
    int     cachedPosCX{0}, cachedPosCZ{0};
    int64_t cachedPosTick{0};

    // 诊断计数（会话结束时汇总成一行，让人一眼看懂这次传送到底经历了什么）
    int         statPoolMiss{0};      // 池没给点（未就绪/圆内无点）
    int         statUnsafeRounds{0};  // 候选点判定为"整块无安全列"的次数
    int         statNoDataRounds{0};  // 候选点需要生成（存档里没有）的次数
    int         statExpandMax{0};     // 扩圈最深到第几圈
    int         statGenWaits{0};      // 等区块生成的轮次
    int         statGenTicks{0};      // 等区块生成累计拍数（含邻域等待，见汇总行）
    int         statNbrTicks{0};      // 其中"等周围一圈"的拍数
    int         statMemScan{0};       // 走内存扫描判定安全的次数
    int         statTableHit{0};      // 走落点表判定安全的次数
    int         statArchiveHit{0};    // 走存档直读判定安全的次数

    std::unordered_set<std::string> dangerSet;      // 全名（minecraft:xxx, BlockSource 用）
    std::unordered_set<std::string> dangerShortSet; // 短名（无前缀, 存档 palette 用）

    // 状态机: PROBE(三级数据源探测) → SCAN_CHUNK(落点 chunk 全列扫) → EXPAND(扩圈)
    //         PROBE miss → LOAD_CHUNK(TickingArea 生成等待) → SCAN_CHUNK
    enum State { PROBE, LOAD_CHUNK, SCAN_CHUNK, EXPAND } state{PROBE};
    int  chunkWait{0};   // 生成路径: 单落点区块等待计数
    int  totalTicks{0};  // 会话总 tick（总超时兜底）
    int  titleTick{0};   // actionbar 节流

    int  expandRing{0};  // 扩圈当前半径（chunk; 1..24）
    int  expandIdx{0};   // 圈内周界游标

    // 常加载区域状态（生成路径专用; 存档路径会话全程 areaValid=false）
    std::string areaName;  // "mtpsrtp_<玩家名>_<序号>"（全局唯一, 永不重名）
    int         areaDim{-1};
    int         areaCX{0}, areaCZ{0};  // 中心（方块坐标）
    int         areaRadius{0};          // 半径（区块）
    bool        areaValid{false};
    int         landingHolds{0};       // 落点周围还没加载完而被推迟传送的次数
    // 传送成功后不立刻撤区域: 宽限期交给 GraceArea 队列处理（见 cleanupSessionArea）
    bool        keepAreaAfterTeleport{false};
    int         spawnWaitTicks{0};   // 等待玩家出生流程完成的 tick 数（见 stepSession 开头）

    // 本 tick 的时间片截止点（tick 按会话数平分后写入）。步数预算在混合负载
    // 下不准: 落点表一次 ~ns, 内存扫一块 ~ms, 所以再加一道时间控制。
    std::chrono::steady_clock::time_point deadline{};
    // 本 tick 已经用掉的昂贵配额（跨步累计; 每 tick 开头清零）。
    // 内存扫一块 ≈1ms, 所以这两个数 = 每 tick 给这个会话的主线程耗时上限。
    int tickScanUsed{0};   // 内存扫块数
    int tickIoUsed{0};     // 存档直读块数
    // 会话开始时刻（日志里报真实耗时, debug 用）
    std::chrono::steady_clock::time_point startedAt{};

    bool finished{false};
};

// 某个时刻到现在过了多久（毫秒）。接时间点而不是 Session: Session 是私有嵌套类型。
static int64_t msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
}

static std::mt19937_64& rng() { static std::mt19937_64 r{std::random_device{}()}; return r; }

// 常加载区域管理（成员: 需要读写 Session 私有字段）

// 确保会话的常加载区域覆盖 (blockX, blockZ) 为中心、radiusChunks 为半径的圆
// 区域名每次换区都带新序号 → 撤旧建新（同 tick）不会名字冲突
void RandomTeleport::ensureTickingArea(Session& s, Level& level, int blockX, int blockZ, int radiusChunks) {
    // 现有区域盖得住这个位置就什么都不做。以前这里按"方块坐标完全相同"判断，落点差 1 格就
    // 重新登记：先撤旧区域，覆盖这些区块的引用立刻没了，引擎当场把刚生成好的区块卸掉（实测
    // 日志里 状态=Loaded 隔一拍变 Unloaded），新区域又让它从头加载，卡在 StructPostProcDone
    // 直到 200 tick 超时——一次传送白等 10 秒。区域是圆、有半径，差几个方块不影响它兜住同一批块。
    if (s.areaValid && s.areaDim == s.dimid && s.areaRadius >= radiusChunks) {
        int const dcx   = (blockX >> 4) - (s.areaCX >> 4);
        int const dcz   = (blockZ >> 4) - (s.areaCZ >> 4);
        int const slack = s.areaRadius - radiusChunks;   // 还能偏几个区块、仍被完全覆盖
        if (std::abs(dcx) <= slack && std::abs(dcz) <= slack) return;
    }

    // ② 真要换地方: 先挂新区域、再撤旧的 —— 顺序不能反, 中间空一拍区块就会被卸掉。
    //    旧区域延后撤（新区块与旧的覆盖范围可能重叠, 同 tick 撤会把重叠部分一起卸掉）。
    std::string const oldName = s.areaName;
    bool const        hadOld  = s.areaValid;
    int const         oldCX   = s.areaCX, oldCZ = s.areaCZ;

    s.areaName = makeAreaName(s.playerName); // 新序号名（永不重名, 无冲突窗口）
    // 直接登记引擎的常加载区域（不落盘、无残留; 区域内的区块由引擎每 tick 加载/生成）
    auto const st = addRtpArea(level, s.dimid, s.areaName, blockX, blockZ, radiusChunks);
    if (st == ::AddTickingAreaStatus::Success) {
        s.areaValid  = true;
        s.areaDim    = s.dimid;
        s.areaRadius = radiusChunks;
        s.areaCX     = blockX;
        s.areaCZ     = blockZ;
        if (hadOld) {
            scheduleAreaRemoval(s.playerName, oldName, s.dimid, RTP_AREA_HANDOVER_TICKS,
                                oldCX >> 4, oldCZ >> 4);
            RTP_DBG("[RTP][区域] 换点: 先挂新区域 {} @({}, {}), 旧的 {} 延后 {} tick 撤",
                    s.areaName, blockX, blockZ, oldName, RTP_AREA_HANDOVER_TICKS);
        } else {
            RTP_DBG("[RTP][区域] 已登记常加载区域 circle r={} 区块 @({}, {}) dim={} 名={}",
                    radiusChunks, blockX, blockZ, s.dimid, s.areaName);
        }
    } else {
        // 登记失败: 保留旧区域与它的名字（别把已有的引用丢掉, 那正好会卸掉区块）
        s.areaName  = hadOld ? oldName : std::string{};
        s.areaValid = hadOld;
        rtpLogger().warn("[RTP] 常加载区域登记失败(status={}): 中心 ({}, {}) r={} 区块 dim={}"
                         "（本次传送可能超时）", (int)st, blockX, blockZ, radiusChunks, s.dimid);
    }
}

// 会话收尾: 移除常加载区域（结束/超时/离线/作废/关服统一走这里）。
// 传送成功后不能立刻撤: 靠本区域才加载的地块, 同 tick 撤掉会让客户端收不到区块数据（灰屏）,
// 所以留 RTP_AREA_GRACE_TICKS 宽限期等玩家视野接管。
void RandomTeleport::cleanupSessionArea(Session& s) {
    if (!s.areaValid) return;
    s.areaValid = false;
    auto level = ll::service::getLevel();
    if (!level) return; // 关服末期拿不到 Level: 残留区域由下次启动的前缀清理兜底

    if (s.keepAreaAfterTeleport) {
        scheduleAreaRemoval(s.playerName, s.areaName, s.areaDim, RTP_AREA_GRACE_TICKS,
                            s.areaCX >> 4, s.areaCZ >> 4);
        RTP_DBG("[RTP][区域] 传送成功, {} 保留 {} tick 宽限期 (dim={})",
                s.areaName, RTP_AREA_GRACE_TICKS, s.areaDim);
        return;
    }
    removeRtpArea(*level, s.areaDim, s.areaName);
    RTP_DBG("[RTP][区域] 会话结束, 移除 {} (dim={})", s.areaName, s.areaDim);
}

// 宽限期待撤区域
void RandomTeleport::scheduleAreaRemoval(std::string const& playerName, std::string const& name,
                                         int dim, int delayTicks, int landingCX, int landingCZ) {
    mGraceAreas.push_back(
        GraceArea{name, playerName, dim, mTickCounter + delayTicks, landingCX, landingCZ});
}

void RandomTeleport::processGraceAreas() {
    if (mGraceAreas.empty()) return;
    auto level = ll::service::getLevel();
    for (auto it = mGraceAreas.begin(); it != mGraceAreas.end();) {
        if (mTickCounter < it->removeAtTick) { ++it; continue; }

        if (!it->removed) {
            if (level) removeRtpArea(*level, it->dim, it->name);
            it->removed       = true;
            it->removeAtTick  = mTickCounter + 20;   // 撤完再等 20 tick 做自检
            ++it;
            continue;
        }

        // 自检: 区域撤掉之后, 落点区块还应该是 Loaded（说明玩家自己的视野已经接管了它）。
        // 若这里读到 Unloaded, 就是"传送后一片灰"的病态情况仍然存在 —— 这条日志是
        // 判断根因是否真的修掉的关键证据（debug 开启时输出）。
        if (level) {
            auto dim = level->getDimension((::DimensionType)it->dim).lock();
            if (dim) {
                ChunkState st = chunkStateAt(*dim, it->lcx << 4, it->lcz << 4);
                // 玩家已离线 / 已跑远 → 区块卸载是正常的（没有视野引用它），不能按"灰屏"报警。
                // 实测这条虚警把"玩家传送后 4 秒断线"误报成了"会出现灰屏"。
                bool nearby = false;
                if (auto* p2 = level->getPlayer(it->playerName)) {
                    auto const pos = p2->getPosition();
                    nearby = (p2->getDimensionId() == (::DimensionType)it->dim)
                          && std::abs((int)pos.x - (it->lcx << 4)) < 128
                          && std::abs((int)pos.z - (it->lcz << 4)) < 128;
                }
                RTP_DBG("[RTP][自检] 撤区域后落点区块({},{}) 状态={}{}",
                        it->lcx, it->lcz, chunkStateName(st),
                        st >= ChunkState::Loaded ? "（玩家视野已接管, 正常）"
                        : !nearby                 ? "（玩家已离线/离开该处, 卸载属正常）"
                                                  : "（未被接管, 会出现灰屏）");
            }
        }
        it = mGraceAreas.erase(it);
    }
}

// 落点是否已被会话的常加载区域覆盖
// 区域是圆的, 所以按欧氏距离比。用切比雪夫（取 dx/dz 最大值）测的是正方形,
// 正方形面积大于圆, 角落处会把圆外的方块判成"已覆盖" —— 那正是我们不想漏判的方向。
bool RandomTeleport::areaCoversLanding(Session const& s, SafePos const& p) {
    if (!s.areaValid || s.areaDim != p.dimid) return false;
    int const dx = ((int)std::floor(p.x) >> 4) - (s.areaCX >> 4);
    int const dz = ((int)std::floor(p.z) >> 4) - (s.areaCZ >> 4);
    return dx * dx + dz * dz <= s.areaRadius * s.areaRadius;
}

// 发起
// 各玩家在各预设上的上次发起时刻（键 = 玩家名 + 预设名）
static std::unordered_map<std::string, int64_t>& rtpCooldowns() {
    static std::unordered_map<std::string, int64_t> m;
    return m;
}

void RandomTeleport::start(Player& player, RtpOptions const& opts) {
    // 冷却: 预设自己设了就用它的, 否则用全局 randomTeleport.cooldownSeconds; <0 = 不检查
    // （调用方自带冷却的入口用 <0, 例如 NPC 随机传送点 —— 那个点有自己的冷却, 不该再被全局卡一次）。
    // 记账键必须带预设身份: 只用消息文本的话, 消息相同的多个预设会共用一条记录, 互相干扰。
    int cooldown = opts.cooldownSeconds;
    if (cooldown == 0) cooldown = Config::getInstance().randomCooldownSeconds();
    if (cooldown > 0) {
        std::string const key = player.getRealName() + "|" +
                                (opts.presetName.empty() ? opts.message : opts.presetName);
        int64_t           now = (int64_t)std::time(nullptr);
        auto              it  = rtpCooldowns().find(key);
        if (it != rtpCooldowns().end()) {
            int64_t const left = cooldown - (now - it->second);
            if (left > 0) {
                player.sendMessage("§c[随机传送] §f冷却中, 还需等待 §e" + std::to_string(left) + "§f 秒");
                return;
            }
        }
        rtpCooldowns()[key] = now;
    }

    auto s = std::make_shared<Session>();
    s->startedAt = std::chrono::steady_clock::now();
    s->playerName = player.getRealName();
    s->dimid      = opts.dimid;
    s->message    = opts.message.empty() ? "§a[随机传送] §f已传送！" : opts.message;
    s->cost       = opts.cost;
    // 候选点重试名额 = config 的 maxAttempts。这个键以前只被读取、从未生效（重试次数写死在
    // 会话里的 6），配成 200 也还是只试 6 次。上限 16：每次尝试都可能要生成一整个区块
    // （r=2 的区域 = 25 块），无上限会把生成队列塞爆；会话 60 秒总超时仍是兜底。
    s->reRandomLeft = std::min(16, std::max(1, Config::getInstance().randomMaxAttempts()));
    s->radius     = std::max(1, opts.radius);
    s->usePlayerOrigin = (opts.originMode == "player");
    s->originX = opts.originX;
    s->originZ = opts.originZ;
    if (s->usePlayerOrigin) { // 发起那一刻记下原点
        s->originX = std::floor(player.getPosition().x);
        s->originZ = std::floor(player.getPosition().z);
    }
    s->yRange     = getDimYRange(s->dimid);
    s->scanStartY = std::min(getScanStartY(s->dimid), s->yRange.maxY);
    for (auto& b : Config::getInstance().dangerBlocks()) s->dangerSet.insert(b);          // 全名（BlockSource 用）
    for (auto& b : Config::getInstance().dangerShortBlocks()) s->dangerShortSet.insert(b); // 短名（存档 palette 用）

    RTP_DBG("[RTP][会话] 开始: 玩家={} 维度={} 原点=({},{}) 模式={} 半径={} 存档就绪={}",
            s->playerName, s->dimid, (int)s->originX, (int)s->originZ,
            s->usePlayerOrigin ? "玩家坐标" : "固定", s->radius,
            ArchiveScanner::getInstance().ready() ? "是" : "否(索引建立中)");

    // 经济预扣（找不到安全位置时退还）
    if (s->cost > 0 && Config::getInstance().economyEnabled()) {
        if (!Economy::getInstance().withdraw(player, s->cost)) {
            player.sendMessage("§c[随机传送] §f余额不足，无法传送");
            return;
        }
    }

    // 同一玩家旧会话作废（防连点叠加; 旧会话区域一并移除）
    for (auto it = mSessions.begin(); it != mSessions.end();) {
        if ((*it)->playerName == s->playerName) {
            cleanupSessionArea(**it);
            it = mSessions.erase(it);
        } else {
            ++it;
        }
    }
    pickNewTarget(*s);
    mSessions.push_back(s);
}

// 选新随机落点（玩家原地不动, 只更新会话目标）
void RandomTeleport::pickNewTarget(Session& s) {
    // ① 预落点池优先：池里的点是预先算好的（已加载类 = 存档内已知安全点；未加载类 =
    //    cubiomes 按世界种子推算的候选点）。命中即用 —— 命中率与半径无关（内部按空间桶采样，
    //    不是对整张表做拒绝采样）。
    if (tryPoolLanding(s)) return;

    // ② 池不可用/未命中 → 原有链路（这就是"降级策略"，行为与改造前完全一致）
    // 配置开了就先试"存档里已知安全"的已生成地块（落点确定、不用等地形生成）
    if (Config::getInstance().randomPreferKnown() && tryKnownLanding(s)) return;

    // 圆内随机挑点（√r 均匀分布），避开之前失败的落点
    int avoidDist = std::min(640, std::max(128, s.radius / 4));
    std::uniform_real_distribution<double> dist01(0, 1);
    double angle = 0, dist = 0;
    // 群系预筛（降级路径也享受）：均匀随机点里约三成落在海洋/河流上，那种点必然要
    // 白生成一整个区块、再判定"整块无安全列"（实测白等 5.8 秒）。先在群系图上筛掉它，
    // 成本是每次约 0.1ms 的单点采样、最多 4 次。采样器没配好（极早期/池关闭）就跳过。
    int  biomeFilters = 0;
    bool biomeFiltered = false;
    for (int tries = 0; tries < 12; tries++) {
        angle = dist01(rng()) * kPi * 2;
        dist  = std::sqrt(dist01(rng())) * s.radius;
        double cx = s.originX + std::cos(angle) * dist;
        double cz = s.originZ + std::sin(angle) * dist;
        bool tooClose = false;
        for (auto& t : s.triedTargets) {
            double dx = t.first - cx, dz = t.second - cz;
            if (dx*dx + dz*dz < (double)avoidDist*avoidDist) { tooClose = true; break; }
        }
        if (!tooClose && biomeFilters < 4) {
            auto& sampler = BiomeSampler::getInstance();
            if (sampler.ready()) {
                auto const sr = sampler.sample((int)std::lround(cx), (int)std::lround(cz));
                biomeFilters++;
                if (sr.ok && sr.excluded) {
                    biomeFiltered = true;
                    tooClose      = true;   // 复用同一个"换一个"分支
                }
            }
        }
        if (!tooClose) break;
        if (tries == 11) { angle = dist01(rng()) * kPi * 2; dist = std::sqrt(dist01(rng())) * s.radius; }
    }
    s.randomX = (int)std::round(s.originX + std::cos(angle) * dist);
    s.randomZ = (int)std::round(s.originZ + std::sin(angle) * dist);
    s.triedTargets.push_back({s.randomX, s.randomZ});

    RTP_DBG("[RTP][落点] #{} ({}, {}) 距原点{}格{}", s.triedTargets.size(), s.randomX, s.randomZ,
            (int)std::round(dist), s.triedTargets.size() > 1
                ? "（重随, 剩余名额" + std::to_string(s.reRandomLeft) + "）" : "");
    if (biomeFiltered) {
        RTP_DBG("[RTP][落点] 已按群系预筛掉 {} 个落点（海洋/河流等被排除群系, 那些点会白生成一次）",
                biomeFilters);
    }

    s.state        = Session::PROBE;
    s.chunkWait    = 0;
    s.expandRing   = 0;
    s.expandIdx    = 0;
    s.landingHolds = 0;
    s.haveCachedPos = false;   // 换了目标 → 上次缓存的落点作废
}

// 从预落点池抽一个候选点当落点：已加载类是落点表来的已知安全点（只需载入区块），
// 未加载类是 cubiomes 推的候选点（需要生成，这是唯一允许的等待）。
// pick() 返回的坐标就是传送目标，回写状态也用同一坐标。
bool RandomTeleport::tryPoolLanding(Session& s) {
    auto& pool = PreLandingPool::getInstance();
    if (!pool.usable()) {
        // 池不可用（未启用/还没补齐/该维度没有点）→ 静默降级。但要把原因说出来，
        // 否则"为什么没走快路径"从日志上完全看不出来。
        s.statPoolMiss++;
        RTP_DBG("[RTP][预落点池] 不可用（未启用 / 还没补齐 / 维度{}没有点）→ 本次走降级链路",
                s.dimid);
        return false;
    }

    PreLandingPool::Pick pick{};
    // 同一玩家上次落点作为"间隔约束"（池里没有更远的点时会自动放开, 不会因此失败）
    int const  avoidR = Config::getInstance().landingPoolHitRadius();
    auto const it     = sLastLanding().find(s.playerName);
    int const  avX    = (it != sLastLanding().end()) ? it->second.first : 0;
    int const  avZ    = (it != sLastLanding().end()) ? it->second.second : 0;
    if (!pool.pick(s.dimid, s.originX, s.originZ, s.radius, rng()(), avX, avZ,
                   it != sLastLanding().end() ? avoidR : 0, pick)) {
        s.statPoolMiss++;
        RTP_DBG("[RTP][预落点池] 圆内（原点 ({:.0f},{:.0f}) 半径 {}）没有可用预落点 → 降级",
                s.originX, s.originZ, s.radius);
        return false;
    }

    s.poolHit    = true;
    s.poolProbed = false;
    s.poolX      = pick.x;
    s.poolZ      = pick.z;
    s.poolTier   = pick.tier;
    s.randomX    = pick.targetX;
    s.randomZ    = pick.targetZ;
    s.triedTargets.push_back({s.randomX, s.randomZ});

    RTP_DBG("[RTP][落点] #{} ({}, {}) 取自预落点池{}（池点即目标, 距原点 {} 格, 群系 {}, 参考高度 {}）",
            s.triedTargets.size(), s.randomX, s.randomZ,
            pick.tier == kPlTierLoaded ? "[已加载类: 存档内已知安全, 只需载入区块]"
                                       : "[未加载类: 待生成, cubiomes 推算]",
            (int)std::lround(std::sqrt((double)(pick.x - s.originX) * (pick.x - s.originX)
                                       + (double)(pick.z - s.originZ) * (pick.z - s.originZ))),
            pick.biome, pick.hintY);

    s.state        = Session::PROBE;
    s.chunkWait    = 0;
    s.expandRing   = 0;
    s.expandIdx    = 0;
    s.landingHolds = 0;
    s.haveCachedPos = false;   // 换了目标 → 上次缓存的落点作废
    return true;
}

// 从存档落点表里抽一个"圆盘半径内、且已有安全落点"的已生成 chunk 当落点。
// 这类区块地形早就生成好了（甚至已在内存里）, 省掉整段"等它生成"的开销。
bool RandomTeleport::tryKnownLanding(Session& s) {
    BedrockLevelReader::Landing ld{};
    if (!ArchiveScanner::getInstance().pickSafeLandingInRange(
            (int)s.originX, (int)s.originZ, s.radius, s.dimid, rng()(), ld)) {
        return false;
    }
    s.randomX = ld.cx * 16 + (ld.lx >= 0 && ld.lx < 16 ? ld.lx : 8);
    s.randomZ = ld.cz * 16 + (ld.lz >= 0 && ld.lz < 16 ? ld.lz : 8);
    s.triedTargets.push_back({s.randomX, s.randomZ});
    RTP_DBG("[RTP][落点] #{} ({}, {}) 取自存档已知安全点 chunk({},{}) y={}（已生成, 不用等地形生成）",
            s.triedTargets.size(), s.randomX, s.randomZ, ld.cx, ld.cz, ld.y);
    s.state        = Session::PROBE;
    s.chunkWait    = 0;
    s.expandRing   = 0;
    s.expandIdx    = 0;
    s.landingHolds = 0;
    s.haveCachedPos = false;   // 换了目标 → 上次缓存的落点作废
    return true;
}

// 一次选点彻底失败: 名额内换点重随; 名额用尽时最后试一次已知安全点; 还是不行才退款放弃
RandomTeleport::StepResult RandomTeleport::retryOrGiveUp(Session& s, Player& p) {
    if (s.reRandomLeft > 0) {
        s.reRandomLeft--;
        pickNewTarget(s);
        return StepResult::Progress;
    }
    if (!s.usedKnownFallback && tryKnownLanding(s)) {
        s.usedKnownFallback = true;
        return StepResult::Progress;
    }
    finishTeleport(s, p, false, nullptr);
    return StepResult::Done;
}

// 落点周围 (2r+1)^2 个区块是否都已加载（只读状态, 不触发加载）。
// 只等落点自己那一块是不够的: 客户端拿到一块孤零零的区块, 视野里其余部分照样是空的。
static bool landingNeighborhoodReady(Dimension& dim, int blockX, int blockZ, int chunkRadius) {
    int const cx = blockX >> 4, cz = blockZ >> 4;
    for (int dx = -chunkRadius; dx <= chunkRadius; dx++) {
        for (int dz = -chunkRadius; dz <= chunkRadius; dz++) {
            if (!isChunkReady(dim, (cx + dx) << 4, (cz + dz) << 4)) return false;
        }
    }
    return true;
}

// 完成（成功: 直接传送到安全点 / 失败: 退款提示, 玩家全程原地）
void RandomTeleport::finishTeleport(Session& s, Player& p, bool success, SafePos const* pos) {
    // 落点是否"真的能落地"。硬条件: 落点区块已加载, 且会话区域覆盖落点。
    // （扩圈命中的落点可能落在区域圆外, 例如 r=3 的角落离圆心 4.24 区块 —— 那时先把区域
    //   挂到落点、退回等待流程, 等它就绪后自然会再走到这里。）
    // 软条件: 区域已被引擎激活（不是还挂在 pending）+ 落点周围 (2*等待半径+1)^2 个区块都已加载。
    // 只满足硬条件就传的话, 玩家会落在"一块孤零零的已加载区块"上: 服务端有方块, 客户端那一圈
    // 还是空的 —— 看起来仍是一片空白。软条件最多压 RTP_LANDING_HOLD_TICKS 拍, 超了也放行。
    if (success && pos) {
        auto level = ll::service::getLevel();
        if (level) {
            auto dim = level->getDimension((::DimensionType)pos->dimid).lock();
            int const bx = (int)std::floor(pos->x);
            int const bz = (int)std::floor(pos->z);
            bool const chunkOk = dim && areaCoversLanding(s, *pos) && isChunkReady(*dim, bx, bz);
            bool const aroundOk = chunkOk
                && s.areaValid && findRtpArea(*level, s.areaDim, s.areaName) != nullptr
                && landingNeighborhoodReady(*dim, bx, bz, RTP_WAIT_AREA_RADIUS_CHUNKS);
            int const holdLimit = Config::getInstance().landingHoldTicks();
            if (!chunkOk || (!aroundOk && s.landingHolds < holdLimit)) {
                // 换落点了才把等待计数清零; 同一落点继续累加（LOAD_CHUNK 的超时要能按期触发,
                // 否则一个卡住的落点会一路拖到会话总超时）
                bool const sameLanding = (s.randomX == bx && s.randomZ == bz);
                if (!sameLanding)      s.chunkWait = 0;
                else if (chunkOk) { s.landingHolds++; s.statNbrTicks++; }
                ensureTickingArea(s, *level, bx, bz, RTP_WAIT_AREA_RADIUS_CHUNKS);
                s.randomX = bx;
                s.randomZ = bz;
                // 缓存这个落点：等"周围区块"的这段里复用它，别每拍重扫一遍
                s.haveCachedPos = true;
                s.cachedPos     = *pos;
                s.cachedPosCX   = bx >> 4;
                s.cachedPosCZ   = bz >> 4;
                s.cachedPosTick = mTickCounter;
                s.state   = Session::LOAD_CHUNK;
                if (s.landingHolds == 1 || s.landingHolds % 10 == 0) {
                    // 把"在等什么"说清楚: 落点区块自身没就绪 / 区域还没被引擎受理 / 周围一圈没加载完
                    bool const areaActive = s.areaValid
                        && findRtpArea(*level, s.areaDim, s.areaName) != nullptr;
                    RTP_DBG("[RTP][落点] 落点区块 ({},{}) 继续等（第{}拍）: {}", bx, bz,
                            s.landingHolds,
                            !chunkOk      ? "落点区块自身未就绪"
                            : !areaActive ? "常加载区域还没激活（引擎尚未受理）"
                                          : "周围区块还没加载完（等 (2r+1)^2 全就绪）");
                }
                return;   // 不结束会话: 交给 LOAD_CHUNK 等
            }
            // 落点复核（debug）: 把落点脚下与身位的方块名打出来。落点质量好不好，
            // 看这一行就够了（落在树冠上会是 *_leaves、落在水里会是 water）。
            // 只有 debug 才读，3 次方块查询，且块已加载。
            if (rtpDebugEnabled()) {
                auto& bs2 = dim->getBlockSourceFromMainChunkSource();
                auto  nm  = [&](int y) -> std::string {
                    return bs2.getBlock(BlockPos(bx, y, bz)).getTypeName();
                };
                RTP_DBG("[RTP][落点复核] ({},{},{}) 脚下={} 脚={} 头={} 头2={}",
                        bx, (int)std::floor(pos->y), bz, nm((int)std::floor(pos->y) - 1),
                        nm((int)std::floor(pos->y)), nm((int)std::floor(pos->y) + 1),
                        nm((int)std::floor(pos->y) + 2));
            }
            // 放行: 另挂一个以落点为中心的宽限区域。
            // 不能像以前那样"把等待区域改挂/扩到落点" —— 那要先撤掉覆盖落点的旧区域,
            // 撤到新区域生效之间有一拍左右没有任何引用, 引擎正好可以卸掉刚加载的区块,
            // 客户端就会收到"区块没了"（表现为一片空白）。所以旧的留着, 之后再一起撤。
            bool const covered = areaCoversLanding(s, *pos) && s.areaRadius >= RTP_GRACE_AREA_RADIUS_CHUNKS;
            if (!covered) {
                std::string const gname = makeAreaName(s.playerName);
                if (addRtpArea(*level, s.dimid, gname, bx, bz, RTP_GRACE_AREA_RADIUS_CHUNKS)
                    == ::AddTickingAreaStatus::Success) {
                    scheduleAreaRemoval(s.playerName, gname, s.dimid, RTP_AREA_GRACE_TICKS, bx >> 4, bz >> 4);
                    RTP_DBG("[RTP][区域] 落点 ({}, {}) 另挂 r={} 宽限区域 {}", bx, bz,
                            RTP_GRACE_AREA_RADIUS_CHUNKS, gname);
                }
            }
        }
    }
    // 真正执行传送放在"结束会话"之前: 玩家此刻正在出生流程 / 跨维度切换里的话不能硬传,
    // 那就退回等待, 下一拍再试（会话总超时兜底）。硬传的后果是玩家被撕成两个实体（幽灵状态）。
    if (success && pos) {
        if (!teleportPlayerIfReady(p, Vec3((float)pos->x, (float)pos->y, (float)pos->z),
                                   (::DimensionType)pos->dimid)) {
            s.state = Session::LOAD_CHUNK;
            RTP_DBG("[RTP][落点] {} 此刻不能传送（出生流程/跨维度切换中）, 暂缓后再试", s.playerName);
            return;   // 不结束会话
        }
        s.keepAreaAfterTeleport = true;   // 区域撤离交给宽限期（见 cleanupSessionArea）
    }
    s.finished = true;
    // 记下落点，供该玩家下次传送做间隔约束（连续两次别落同一片）
    if (success && pos) {
        sLastLanding()[s.playerName] = {(int)std::floor(pos->x), (int)std::floor(pos->z)};
    }
    // 池的自愈全靠这里：未加载类的点生成后扫描通过就升级为已加载类（下次只需载入区块），
    // 判定无安全列或整场失败就标失效、由后台补齐换掉。已加载类不回写——它们本来就来自
    // 落点表，表才是权威，不该被一次超时拉黑。
    // 通知预热器：这个点被用过了 → 释放它的常驻位（区块让引擎按需卸载，位子留给下一个预热点）
    if (s.poolHit) PoolWarmer::getInstance().noteUsed(s.dimid, s.poolX, s.poolZ);
    if (s.poolHit && s.poolProbed && s.poolTier == kPlTierUnloaded) {
        auto& pool = PreLandingPool::getInstance();
        bool const sameChunk = pos && ((int)std::floor(pos->x) >> 4) == (s.poolX >> 4)
                                    && ((int)std::floor(pos->z) >> 4) == (s.poolZ >> 4);
        if (success && sameChunk) {
            pool.markVerified(s.dimid, s.poolX, s.poolZ, (int)std::lround(pos->y));
            RTP_DBG("[RTP][预落点池] 池点 ({},{}) 生成后验证通过 y={} → 升级为已加载类",
                    s.poolX, s.poolZ, (int)std::lround(pos->y));
        } else {
            pool.markInvalid(s.dimid, s.poolX, s.poolZ);
            RTP_DBG("[RTP][预落点池] 池点 ({},{}) {} → 标记失效（后台补齐会补新点）", s.poolX,
                    s.poolZ, success ? "所在 chunk 无安全列" : "本次会话失败");
        }
    }
    if (success && pos) {
        // 下一拍核对客户端的区块发布区域有没有跟到落点（没跟上就是"一片空白"）
        mArrivalChecks.push_back(ArrivalCheck{s.playerName, pos->dimid, pos->x, pos->y, pos->z,
                                              mTickCounter + 1, 0});
        sendActionbar(p, "§a传送成功");
        std::string costTip = (s.cost > 0 && Config::getInstance().economyEnabled())
            ? " §7(花费 §e" + std::to_string(s.cost) + "§7)" : "";
        p.sendMessage(s.message + " §7(" + std::to_string((int)std::floor(pos->x)) + ", " +
                      std::to_string((int)std::floor(pos->y)) + ", " + std::to_string((int)std::floor(pos->z)) + ")" + costTip);
        RTP_DBG("[RTP][结束] {} 成功传送至 ({}, {}, {}) 耗时{}ms/{}tick 落点尝试{}次",
                s.playerName, (int)std::floor(pos->x), (int)std::floor(pos->y), (int)std::floor(pos->z), msSince(s.startedAt), s.totalTicks, s.triedTargets.size());
    } else {
        sendActionbar(p, "§c没有找到安全位置");
        if (s.cost > 0 && Config::getInstance().economyEnabled()) {
            Economy::getInstance().deposit(p, s.cost);
            p.sendMessage("§c[随机传送] §f没有找到安全位置 §7(已退还 §e" + std::to_string(s.cost) + "§7)");
            RTP_DBG("[RTP][结束] {} 失败(未找到安全位置, 已退款{}) 耗时{}ms/{}tick 落点尝试{}次",
                    s.playerName, s.cost, msSince(s.startedAt), s.totalTicks, s.triedTargets.size());
        } else {
            p.sendMessage("§c[随机传送] §f没有找到安全位置");
            RTP_DBG("[RTP][结束] {} 失败(未找到安全位置) 耗时{}ms/{}tick 落点尝试{}次",
                    s.playerName, msSince(s.startedAt), s.totalTicks, s.triedTargets.size());
        }
    }
    // 会话汇总（一行看懂"这次传送到底经历了什么"）: 候选从哪来、试了几轮、扩圈多深、
    // 每种判定各走了几次、等了几轮生成。排查"随机传送很烂"就从这一行开始 ——
    // 它把前面几十条过程日志压成了结论（耗时/尝试次数见上面的 [RTP][结束] 行）。
    RTP_DBG("[RTP][汇总] {} {} | 候选{}轮（其中池未命中{}轮） | 无安全列{}次 需生成{}次 扩圈最深{}圈 | "
            "安全列来源: 内存{} 落点表{} 存档直读{} | 等生成{}轮 {}拍（其中等周围一圈 {}拍）",
            s.playerName, success ? "成功" : "失败", (int)s.triedTargets.size(), s.statPoolMiss,
            s.statUnsafeRounds, s.statNoDataRounds, s.statExpandMax, s.statMemScan, s.statTableHit,
            s.statArchiveHit, s.statGenWaits, s.statGenTicks, s.statNbrTicks);
}

// 单 chunk 判定（三源逐级）: Safe = 找到安全列 / Unsafe = 有数据但整 chunk 无安全列 /
// NoData = 内存与存档都查不到 → 需走生成路径。
RandomTeleport::StepResult RandomTeleport::stepProbe(Session& s, Level& level, Player& p, Dimension& dim) {
    int cx = s.randomX >> 4, cz = s.randomZ >> 4;
    SafePos     pos{};
    std::string reason;
    bool        cheap = false;
    ChunkSource src   = ChunkSource::None;
    auto const  verdict = resolveChunk(dim, s.dimid, cx, cz, s.yRange, s.scanStartY,
                                       s.dangerSet, s.dangerShortSet, pos, reason, cheap, &src);
    // 池点已经过判定 → 允许在收尾时回写"失效"（池的自愈靠它）
    if (s.poolHit) s.poolProbed = true;
    switch (verdict) {
        case ChunkVerdict::Safe:
            if (src == ChunkSource::Memory)       s.statMemScan++;
            else if (src == ChunkSource::Table)   s.statTableHit++;
            else if (src == ChunkSource::Archive) s.statArchiveHit++;
            RTP_DBG("[RTP][探测] #{} ({},{}) {} 命中安全列 ({}, {}, {})",
                    s.triedTargets.size(), s.randomX, s.randomZ,
                    cheap ? "落点表" : "内存/直读", blockXOf(pos.x), (int)pos.y, blockXOf(pos.z));
            finishTeleport(s, p, true, &pos);
            return StepResult::Done;
        case ChunkVerdict::Unsafe:
            s.statUnsafeRounds++;
            RTP_DBG("[RTP][探测] #{} ({},{}) {} → 进入扩圈搜索",
                    s.triedTargets.size(), s.randomX, s.randomZ, reason);
            s.state      = Session::EXPAND;
            s.expandRing = 1;
            s.expandIdx  = 0;
            return StepResult::Progress;
        case ChunkVerdict::NoData:
        default:
            s.statNoDataRounds++;
            RTP_DBG("[RTP][探测] #{} ({},{}) {} → TickingArea 生成兜底",
                    s.triedTargets.size(), s.randomX, s.randomZ, reason);
            // 除了登记常加载区域, 再直接向引擎请求这个区块（Deferred: 允许异步生成）
            requestChunkLoad(s.dimid, s.randomX, s.randomZ);
            ensureTickingArea(s, level, s.randomX, s.randomZ, RTP_WAIT_AREA_RADIUS_CHUNKS);
            s.state = Session::LOAD_CHUNK;
            return StepResult::Waiting; // 生成中, 下 tick 轮询
    }
}

// 等 TickingArea 把区块推到就绪, 就绪后转去判定
RandomTeleport::StepResult RandomTeleport::stepLoadChunk(Session& s, Level& level, Player& p, Dimension& dim) {
    s.chunkWait++;
    if (s.chunkWait == 1) s.statGenWaits++;   // 诊断: 这次会话等了几轮区块生成
    s.statGenTicks++;                         // 诊断: 一共等了多少拍
    if (s.chunkWait > RTP_CHUNK_WAIT_TICKS) {
        // 本落点区块 10 秒仍未生成完成：换点重随或放弃
        RTP_DBG("[RTP][等待] #{} ({},{}) 区块{}tick未就绪(最后状态={}), {}",
                s.triedTargets.size(), s.randomX, s.randomZ, RTP_CHUNK_WAIT_TICKS,
                chunkStateName(chunkStateAt(dim, s.randomX, s.randomZ)),
                s.reRandomLeft > 0 ? "换点重随" : "再试一次已知安全点");
        // 诊断: 把引擎侧的实际情况读回来（是否激活 / 加载模式 / 完成标记 / 区域 bounds）
        if (s.areaValid) {
            auto* area = findRtpArea(level, s.areaDim, s.areaName);
            if (area == nullptr) {
                rtpLogger().info("[RTP][诊断] 区域 {} 不在活动列表（pending={}）",
                                 s.areaName, areaStillPending(level, s.areaDim, s.areaName) ? "是" : "否");
            } else {
                rtpLogger().info("[RTP][诊断] 区域 {} 已激活: loadMode={} preloadDone={} 加载完成={}",
                                 s.areaName, (int)area->getLoadMode(), area->isPreloadDone() ? 1 : 0,
                                 area->getView().isDoneLoading() ? 1 : 0);
            }
        }
        return retryOrGiveUp(s, p);
    }
    if (!isChunkReady(dim, s.randomX, s.randomZ)) {
        // 地形数据在 Loaded 之前就有了，后面还有修饰/光照阶段 —— 实测这两个阶段占掉等待时间的大半。
        // 所以趁早试一次判定: 命中就直接传送, 省掉剩下的等待; 没命中就继续等, 结果不受影响
        // （扫到尚未填充的占位数据只会"找不到安全列", 不会误判成安全）。
        if (chunkStateAt(dim, s.randomX, s.randomZ) >= ChunkState::DecorationPostProcessed
            && (s.chunkWait % 3) == 0) {
            SafePos     earlyPos{};
            std::string earlyReason;
            bool        earlyCheap = false;
            if (resolveChunk(dim, s.dimid, s.randomX >> 4, s.randomZ >> 4, s.yRange, s.scanStartY,
                             s.dangerSet, s.dangerShortSet, earlyPos, earlyReason, earlyCheap)
                == ChunkVerdict::Safe) {
                RTP_DBG("[RTP][提前] #{} ({},{}) 第{}tick 状态={} 已可判定, 不再等 Loaded",
                        s.triedTargets.size(), s.randomX, s.randomZ, s.chunkWait,
                        chunkStateName(chunkStateAt(dim, s.randomX, s.randomZ)));
                finishTeleport(s, p, true, &earlyPos);
                return StepResult::Done;
            }
        }

        ensureTickingArea(s, level, s.randomX, s.randomZ, RTP_WAIT_AREA_RADIUS_CHUNKS); // 区域兜底（正常已就位）
        requestChunkLoad(s.dimid, s.randomX, s.randomZ);   // 每 tick 推一次, 别让引擎队列空着
        if (s.chunkWait % 20 == 1) {
            RTP_DBG("[RTP][等待] #{} ({},{}) 第{}tick 状态={}",
                    s.triedTargets.size(), s.randomX, s.randomZ, s.chunkWait,
                    chunkStateName(chunkStateAt(dim, s.randomX, s.randomZ)));
        }
        return StepResult::Waiting; // 等生成完成, 下 tick 再查
    }
    // 落点缓存命中（同一区块 + 10 拍内扫过）→ 直接用，省掉整块内存扫描（约 1ms/次）。
    // 超过 10 拍会自然落到下面的重扫分支复核一次，兼顾"别每拍重扫"与"数据别太旧"。
    if (s.haveCachedPos && s.cachedPosCX == (s.randomX >> 4) && s.cachedPosCZ == (s.randomZ >> 4)
        && (mTickCounter - s.cachedPosTick) < 10 && isChunkReady(dim, s.randomX, s.randomZ)) {
        // 节流：等待期最长 40 拍，以前每拍打一条 —— 光这两行就是每个传送几十条日志写入。
        // 日志本身也有成本（4.3GB 的日志文件说明这服日志量很大），改成每 5 拍一条。
        if (s.chunkWait <= 1 || (s.chunkWait % 5) == 0) {
            RTP_DBG("[RTP][区块就绪] #{} ({},{}) 等待{}tick 状态={}（复用缓存落点, 不重扫）",
                    s.triedTargets.size(), s.randomX, s.randomZ, s.chunkWait,
                    chunkStateName(chunkStateAt(dim, s.randomX, s.randomZ)));
        }
        finishTeleport(s, p, true, &s.cachedPos);
        return StepResult::Done;
    }
    RTP_DBG("[RTP][区块就绪] #{} ({},{}) 等待{}tick 状态={}",
            s.triedTargets.size(), s.randomX, s.randomZ, s.chunkWait,
            chunkStateName(chunkStateAt(dim, s.randomX, s.randomZ)));
    s.state = Session::SCAN_CHUNK;
    // 区块就绪, 同 tick 内直接接着判定（原来是靠落到下一个 if 实现的, 拆函数后要显式调用）
    return stepScanChunk(s, level, p, dim);
}

// 判定落点区块（内存扫为主, 落点表和存档兜底）
RandomTeleport::StepResult RandomTeleport::stepScanChunk(Session& s, Level& level, Player& p, Dimension& dim) {
    SafePos     pos{};
    std::string reason;
    bool        cheap = false;
    ChunkSource src   = ChunkSource::None;
    int cx = s.randomX >> 4, cz = s.randomZ >> 4;
    if (resolveChunk(dim, s.dimid, cx, cz, s.yRange, s.scanStartY,
                     s.dangerSet, s.dangerShortSet, pos, reason, cheap, &src) == ChunkVerdict::Safe) {
        // 来源标出来: 落点表出来的落点也要走内存复核（表可能是上次开服存下来的,
        // 玩家这期间改过地形就不能照搬）, 这一行能看到到底复核的是哪来的数据
        RTP_DBG("[RTP][落点判定] chunk({},{}) 命中安全列 ({}, {}, {}) 来源={}",
                cx, cz, blockXOf(pos.x), (int)pos.y, blockXOf(pos.z),
                src == ChunkSource::Memory ? "内存(实时)" :
                src == ChunkSource::Table  ? "存档落点表(已复核)" : "存档直读");
        finishTeleport(s, p, true, &pos);
        return StepResult::Done;
    }
    s.statUnsafeRounds++;
    RTP_DBG("[RTP][落点判定] chunk({},{}) 不安全: {} → 进入扩圈搜索", cx, cz, reason);
    s.state      = Session::EXPAND;
    s.expandRing = 1;
    s.expandIdx  = 0;
    return StepResult::Progress;
}

// 扩圈搜索: 一圈一圈往外找安全列。昂贵判定（内存扫 / 存档直读）按 tick 配额限,
// 落点表查询极便宜（一次二分）可以放心多跑; 每 tick 用完配额就把游标存下来下 tick 接着扫。
RandomTeleport::StepResult RandomTeleport::stepExpand(Session& s, Level& level, Player& p, Dimension& dim) {
    if (s.expandRing > s.statExpandMax) s.statExpandMax = s.expandRing;   // 诊断: 扩圈最深到哪
    auto& ring = expandRing(s.expandRing);
    int cheapCnt = 0;   // 本 tick 落点表命中次数（独立、宽得多的预算）
    int pending = 0;    // 区内未就绪 chunk 数（等生成, 不耗预算）
    // 未命中的来源拆分（诊断: 区分"这片是大洋"和"这片压根没生成"）
    int memMiss = 0, tableMiss = 0, noDataMiss = 0;

    // 存档里这一维度一个区块都没有（全新世界 / 表还没就绪）: 区外全是"无数据",
    // 扫完几十圈也只是白跑, 直接按"圈耗尽"处理。
    auto& arch = ArchiveScanner::getInstance();
    bool const archiveEmpty = arch.landingsReady() && arch.landingCountForDim(s.dimid) == 0;
    bool const inAreaRing    = s.areaValid && s.expandRing <= s.areaRadius;
    if (archiveEmpty && !inAreaRing) {
        RTP_DBG("[RTP][扩圈] 存档里维度{}没有任何区块, 区外不必再扫", s.dimid);
        return retryOrGiveUp(s, p);
    }

    int const ccx0 = s.randomX >> 4, ccz0 = s.randomZ >> 4;
    int i = s.expandIdx;
    for (; i < (int)ring.size(); i++) {
        if (cheapCnt >= RTP_TABLE_CHUNKS_PER_TICK) break;
        // 昂贵配额用完了: 本 tick 到此为止（游标存着, 下 tick 接着扫 —— 不跳过任何区块）。
        // 时间片也一起看: 配额是按"块数"估的, 遇到特别大的区块不至于超支太多。
        bool const scanFull = s.tickScanUsed >= RTP_SCAN_CHUNKS_PER_TICK;
        bool const ioFull   = s.tickIoUsed >= RTP_IO_CHUNKS_PER_TICK;
        bool const overTime = (s.tickScanUsed > 0 || s.tickIoUsed > 0)
                              && std::chrono::steady_clock::now() > s.deadline;
        if (overTime || scanFull || ioFull) break;
        int ccx = ccx0 + ring[i].dx;
        int ccz = ccz0 + ring[i].dz;
        // 区内（到圆心距离 ≤ 区域半径的圆内）但未就绪: 等生成后重扫本圈
        bool const inArea = s.areaValid
            && (ring[i].dx * ring[i].dx + ring[i].dz * ring[i].dz <= s.areaRadius * s.areaRadius);
        if (inArea && !isChunkReady(dim, ccx << 4, ccz << 4)) { pending++; continue; }
        // 判定（内部三源逐级: 内存优先 → 落点表 → 存档直读; 查不到 = 未生成的 chunk）
        SafePos     pos{};
        std::string reason;
        bool        cheap = false;
        ChunkSource src   = ChunkSource::None;
        ChunkVerdict v = resolveChunk(dim, s.dimid, ccx, ccz, s.yRange, s.scanStartY,
                                      s.dangerSet, s.dangerShortSet, pos, reason, cheap, &src);
        if (cheap) cheapCnt++;
        else if (src == ChunkSource::Archive) s.tickIoUsed++;   // 读盘+解压, 单独一条配额
        else                                  s.tickScanUsed++;  // 内存扫
        if (v == ChunkVerdict::Safe) {
            RTP_DBG("[RTP][扩圈] r={} chunk({},{}) 命中安全列 ({}, {}, {})",
                    s.expandRing, ccx, ccz, blockXOf(pos.x), (int)pos.y, blockXOf(pos.z));
            finishTeleport(s, p, true, &pos);
            return StepResult::Done;
        }
        if (v == ChunkVerdict::NoData) noDataMiss++;
        else if (src == ChunkSource::Memory) memMiss++;
        else tableMiss++;
    }
    s.expandIdx = i;

    if (i < (int)ring.size()) return StepResult::Progress;        // 本圈没扫完: 下 tick 继续（预算控制）
    if (pending > 0) {                          // 本圈扫完但区内有 chunk 仍在生成: 等下 tick 重扫本圈
        RTP_DBG("[RTP][扩圈] r={} 圈扫完, {}个区内chunk生成中, 下tick重扫", s.expandRing, pending);
        s.expandIdx = 0;
        return StepResult::Waiting;
    }
    // 本圈彻底扫完
    RTP_DBG("[RTP][扩圈] r={} 圈扫描完毕(未命中: 内存{} 落点表{} 无数据{})",
            s.expandRing, memMiss, tableMiss, noDataMiss);
    if (s.expandRing < RTP_EXPAND_MAX_RING) {
        s.expandRing++;
        s.expandIdx = 0;
        return StepResult::Progress;
    }
    // 全部圈耗尽: 大洋中央/大片未生成区域 → 换点重随
    RTP_DBG("[RTP][扩圈] 全部{}圈耗尽", RTP_EXPAND_MAX_RING);
    return retryOrGiveUp(s, p);
}

RandomTeleport::StepResult RandomTeleport::stepSession(Session& s) {
    if (s.finished) return StepResult::Done;
    s.totalTicks++;

    auto level = ll::service::getLevel();
    if (!level) { s.finished = true; return StepResult::Done; }
    Player* p = level->getPlayer(s.playerName);
    if (!p) { // 玩家离线：清会话
        RTP_DBG("[RTP][中断] {} 玩家离线, 会话清理", s.playerName);
        s.finished = true; return StepResult::Done;
    }

    // 会话总超时（60 秒兜底: 生成异常/死循环保护）
    if (s.totalTicks > RTP_SESSION_TIMEOUT_TICKS) {
        RTP_DBG("[RTP][超时] {} 会话总超时(60秒), 放弃传送", s.playerName);
        finishTeleport(s, *p, false, nullptr);
        return StepResult::Done;
    }

    // actionbar 节流（约 500ms 一次）。
    // 带上"当前在等什么 + 已经等了多久"：随机传送最慢的一段是等引擎生成区块（实测 1.4~5.8 秒），
    // 以前这里只有一句不动的"随机传送中......" —— 玩家看不出是在工作还是卡住了。
    if (s.titleTick <= 0) {
        int const sec = (int)(msSince(s.startedAt) / 1000);
        char      buf[160];
        switch (s.state) {
            case Session::LOAD_CHUNK: {
                // 区块状态名是英文枚举，只给阶段与"第几 tick"，玩家看得懂"生成中"
                std::snprintf(buf, sizeof(buf),
                              "§e随机传送中… §7目标区块生成中 §8已等 %ds §7(%d tick)", sec,
                              s.chunkWait);
                break;
            }
            case Session::SCAN_CHUNK:
            case Session::EXPAND:
                std::snprintf(buf, sizeof(buf), "§e随机传送中… §7正在找安全落脚点 §8(%ds)", sec);
                break;
            default:
                std::snprintf(buf, sizeof(buf), "§e随机传送中… §7已选好落点 §8(%ds)", sec);
                break;
        }
        sendActionbar(*p, buf);
        s.titleTick = 10;
    }
    s.titleTick--;

    // 出生流程未完成不传送（否则引擎会把玩家放回出生点, 客户端一片灰; 立刻 /tpr 即复现）。
    // 跨维度切换中也不能传（会把玩家撕成两个实体 → 幽灵状态, 能刷物）。
    // 上限 RTP_SPAWN_WAIT_TICKS: 超时就当异常放行, 免得死等（后面 finishTeleport 还会再挡一次）。
    bool const spawnPending = !isPlayerSpawned(*p);
    bool const transferring = isPlayerInDimensionTransfer(*p);
    if ((spawnPending || transferring) && s.spawnWaitTicks < RTP_SPAWN_WAIT_TICKS) {
        s.spawnWaitTicks++;
        if (s.spawnWaitTicks == 1) {
            RTP_DBG("[RTP][等待] {} {} 暂缓传送", s.playerName,
                    transferring ? "正在切换维度" : "出生流程未完成");
        }
        return StepResult::Waiting;
    }

    auto dim = level->getDimension((::DimensionType)s.dimid).lock();
    if (!dim) { finishTeleport(s, *p, false, nullptr); return StepResult::Done; }

    // 状态 1: PROBE（新落点判定: 内存/落点表/存档直读, 单 tick 完成）
    // 只做分发, 各状态自己的逻辑在 stepXxx 里
    switch (s.state) {
        case Session::PROBE:      return stepProbe(s, *level, *p, *dim);
        case Session::LOAD_CHUNK: return stepLoadChunk(s, *level, *p, *dim);
        case Session::SCAN_CHUNK: return stepScanChunk(s, *level, *p, *dim);
        case Session::EXPAND:     return stepExpand(s, *level, *p, *dim);
    }
    s.finished = true;
    return StepResult::Done;
}

// 传送后的"到达到位"核对。
// 服务端把区块加载好 ≠ 客户端拿到了区块: 每客户端由 NetworkChunkPublisher 记账"已经发给它
// 哪些区块", 记录的中心还停在传送前的位置时, 客户端就一直是空的（这正是"一片空白"的由来）。
// 引擎正常会在下一拍自己跟过去; 没有跟过去就由我们清一次区域, 逼它按新位置重新发布。
void RandomTeleport::processArrivalChecks() {
    if (mArrivalChecks.empty()) return;
    auto level = ll::service::getLevel();
    if (!level) { mArrivalChecks.clear(); return; }

    for (auto it = mArrivalChecks.begin(); it != mArrivalChecks.end();) {
        if (mTickCounter < it->atTick) { ++it; continue; }
        Player* p = level->getPlayer(it->playerName);
        if (!p) { it = mArrivalChecks.erase(it); continue; }
        auto& pub = p->mChunkPublisherView;
        if (!pub) { it = mArrivalChecks.erase(it); continue; }

        int const needCX = (int)std::floor(it->x) >> 4;
        int const needCZ = (int)std::floor(it->z) >> 4;
        auto*     lastPub = pub->mLastChunkUpdatePosition.operator->();   // TypedStorage 取值
        int const haveCX = lastPub->x >> 4;
        int const haveCZ = lastPub->z >> 4;
        int const r      = std::max(2, (int)pub->mLastChunkUpdateRadius);
        if (std::abs(haveCX - needCX) <= r && std::abs(haveCZ - needCZ) <= r) {
            it = mArrivalChecks.erase(it);   // 已经跟过去了, 正常
            continue;
        }
        pub->clearRegion();   // 客户端那边还是旧区域: 清掉记账, 让引擎下一拍按新位置重发
        RTP_DBG("[RTP][发布] {} 的区块发布区域还停在 chunk({},{})（落点 chunk({},{})）→ 已清空重发（第{}次）",
                it->playerName, haveCX, haveCZ, needCX, needCZ, it->tries + 1);
        if (++it->tries >= RTP_PUBLISH_MAX_RETRY) { it = mArrivalChecks.erase(it); continue; }
        it->atTick = mTickCounter + RTP_PUBLISH_RECHECK_TICKS;
        ++it;
    }
}

// 调度（多会话轮流推进, 单会话限步数）
void RandomTeleport::tick() {
    mTickCounter++;

    // 一次性预热群系采样器：主线程第一次调用它时要建一份自己的生成器状态（thread_local），
    // 首次约几毫秒 —— 与其让它发生在某次传送的判定里（顶出一帧开销），不如开服后就付掉。
    // 顺带把结果写进 debug 日志：这条日志出现 = "跨线程共用同一个采样器"这条路验证通过。
    if (!mSamplerWarmed) {
        auto& sampler = BiomeSampler::getInstance();
        if (sampler.ready()) {
            auto const sr = sampler.sample(0, 0);
            mSamplerWarmed = true;
            RTP_DBG("[RTP][预热] 主线程群系采样器就绪（原点采样: 群系 {} 是否被排除={} 参考高度 {:.1f}）",
                    sr.biome, sr.excluded, sr.approxY);
        }
    }

    // 首个 tick: 清理上次运行崩溃/异常残留的常加载区域（持久化区域重启会被引擎预加载, 必须兜底）
    static bool sPurged = false;
    if (!sPurged) {
        sPurged = true;
        auto level = ll::service::getLevel();
        if (level) purgeStaleRtpAreas(*level);
    }
    processGraceAreas();   // 传送成功后保留的常加载区域: 宽限期到就撤
    processArrivalChecks();  // 传送后核对客户端有没有真的收到落点周围的区块
    // 时间预算: 按会话数平分, 每个会话至少允许推进一步（否则会被饿死）。
    // 外层还有一道总闸: 会话多的时候"平分"会让总时间超过预算, 所以总耗时到上限就停。
    // 另外: 真正吃时间的"内存扫一个区块"有按 tick 记的配额（见 stepExpand）, 光靠步数限制
    // 会让"6 块 × 8 步"挤在同一 tick 里 —— 那一下就是几十毫秒的 MSPT 尖峰。
    auto const tickStart    = std::chrono::steady_clock::now();
    auto const tickLimit    = tickStart + std::chrono::milliseconds(RTP_TICK_BUDGET_MS);
    int const  sessionCount = mSessions.empty() ? 1 : (int)mSessions.size();
    auto const slice        = std::chrono::milliseconds(RTP_TICK_BUDGET_MS / sessionCount + 1);

    for (auto it = mSessions.begin(); it != mSessions.end();) {
        auto& s = *it;
        // 本 tick 已经用满预算: 剩下的会话留到下个 tick（至少让第一个会话跑完一步）
        if (it != mSessions.begin() && std::chrono::steady_clock::now() > tickLimit) break;
        s->deadline     = tickStart + slice;
        s->tickScanUsed = 0;
        s->tickIoUsed   = 0;
        if (s->finished) { cleanupSessionArea(*s); it = mSessions.erase(it); continue; }
        for (int i = 0; i < RTP_STEPS_PER_TICK && !s->finished; i++) {
            if (i > 0 && std::chrono::steady_clock::now() > s->deadline) break;
            StepResult r = StepResult::Progress;
            try {
                r = stepSession(*s);
            } catch (...) {
                // 异常保护: 结束会话（玩家在原地, 无需回退）
                RTP_DBG("[RTP][异常] {} 会话推进抛异常, 强制结束", s->playerName);
                s->finished = true;
                break;
            }
            if (r != StepResult::Progress) break; // 1=结束 2=等区块: 本会话本 tick 到此为止
        }
        if (s->finished) { cleanupSessionArea(*s); it = mSessions.erase(it); }
        else { ++it; }
    }

    // 本 tick 随机传送实际吃了多少主线程时间（超阈值才报, 用来盯 MSPT 尖峰）
    int64_t const spentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - tickStart).count();
    if (spentMs >= RTP_TICK_WARN_MS) {
        RTP_DBG("[RTP][开销] 本 tick 随机传送占用 {}ms（{} 个会话）", spentMs, sessionCount);
    }
}

// 停止全部会话（玩家全程在原地; 常加载区域一并移除, 关服时防止持久化残留）
void RandomTeleport::stopAll() {
    auto level = ll::service::getLevel();
    for (auto& s : mSessions) {
        if (s->areaValid) {
            s->areaValid = false;
            if (level) removeRtpArea(*level, s->areaDim, s->areaName);
        }
    }
    mSessions.clear();
    // 宽限期待撤的区域也要清掉, 否则它们会被持久化、下次启动才被前缀清理扫到
    if (level) {
        for (auto& ga : mGraceAreas) removeRtpArea(*level, ga.dim, ga.name);
    }
    mGraceAreas.clear();
}

} // namespace mtps
