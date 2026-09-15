// RandomTeleport.cpp - 随机传送（四级数据源 + 区块视野兜底版）
// 会话状态机: 随机选点 → 判定落点 chunk（内存 → 落点预计算表 → 存档直读 → getOrLoadChunk 生成）
// → 整 chunk 无安全列则按 chunk 粒度扩圈(1~24) → 全失败换点重来(最多 6 次) → 仍失败退款。
// 演进过程与各数据源的取舍见 README「随机传送怎么找安全落点」。
#include "RandomTeleportInternal.h"
#include "ArchiveScanner.h"
#include "Config.h"
#include "Economy.h"
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
#include <mc/world/level/block/Block.h>
#include <mc/world/level/dimension/Dimension.h>
#include <mc/world/level/chunk/ChunkSource.h>
#include <mc/world/level/chunk/LevelChunk.h>
#include <mc/world/level/chunk/ChunkState.h>
#include <mc/world/level/ChunkPos.h>
#include <mc/deps/core/math/Vec3.h>
#include <mc/network/packet/SetTitlePacket.h>
#include <mc/network/packet/SetTitlePacketPayload.h>

#include <atomic>
#include <cmath>
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

// Debug 日志（config: randomTeleport.debug, 开启时输出详细流程与时间戳）
// 定义放这里, 声明在 RandomTeleportInternal.h（三个实现文件共用）
ll::io::Logger& rtpLogger() { return ll::mod::NativeMod::current()->getLogger(); }
bool            rtpDebugEnabled() { return Config::getInstance().randomDebug(); }
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

// 区块加载: 见 ChunkLoadUtil.cpp（ChunkSource::getOrLoadChunk, 引擎自己的加载/生成入口）。

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

    int         reRandomLeft{6};
    std::vector<std::pair<int,int>> triedTargets;

    int         randomX{0}, randomZ{0};

    std::unordered_set<std::string> dangerSet;      // 全名（minecraft:xxx, BlockSource 用）
    std::unordered_set<std::string> dangerShortSet; // 短名（无前缀, 存档 palette 用）

    // 状态机: PROBE(三级数据源探测) → SCAN_CHUNK(落点 chunk 全列扫) → EXPAND(扩圈)
    //         PROBE miss → LOAD_CHUNK(等引擎生成) → SCAN_CHUNK
    enum State { PROBE, LOAD_CHUNK, SCAN_CHUNK, EXPAND } state{PROBE};
    int  chunkWait{0};   // 生成路径: 单落点区块等待计数
    int  totalTicks{0};  // 会话总 tick（总超时兜底）
    int  titleTick{0};   // actionbar 节流

    int  expandRing{0};  // 扩圈当前半径（chunk; 1..24）
    int  expandIdx{0};   // 圈内周界游标

    // 区块加载状态（生成路径专用; 存档/内存路径会话全程 loadValid=false）
    bool loadValid{false};             // 本会话是否向引擎请求过加载
    int  loadDim{-1};
    int  loadCX{0}, loadCZ{0};         // 最近请求的区块（避免同一区块重复请求）
    int64_t loadPumpTick{0};           // 下次允许再次请求的时刻

    int  landingCX{-1}, landingCZ{-1}; // 落点区块（传送成功后交给宽限期保持）
    // "从未生成"支路: 把玩家送到目标上方悬停, 由引擎自己把这块地生成出来
    // （JS 版验证过的做法: 玩家一到, 引擎就为他的视野加载/生成周围区块, 不需要"强制生成"接口）
    Vec3 originPos{};                  // 会话开始时玩家的位置（悬停后失败要送回这里）
    int  originDim{-1};
    int  hoverY{0};
    bool hoverMoved{false};
    // 传送成功后不立刻停止请求: 宽限期交给 GraceArea 队列处理（见 cleanupSessionArea）
    bool keepAreaAfterTeleport{false};
    int         spawnWaitTicks{0};   // 等待玩家出生流程完成的 tick 数（见 stepSession 开头）

    // 本 tick 的时间片截止点（tick 按会话数平分后写入）。步数预算在混合负载
    // 下不准: 落点表一次 ~ns, 内存扫一块 ~ms, 所以再加一道时间控制。
    std::chrono::steady_clock::time_point deadline{};
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

// 区块加载管理（成员: 需要读写 Session 私有字段）

// 请求引擎把 (blockX, blockZ) 所在区块弄到内存（见 ChunkLoadUtil: getExistingChunk → createNewChunk）。
// 同一区块 RTP_LOAD_PUMP_TICKS 内不重复请求（生成要时间, 每次调用都是引擎侧的一次查询）。
// 返回 false = 被拒（坐标越界 / 引擎不接受）: 这不是"生成慢", 等下去也没用, 调用方应当换点。
bool RandomTeleport::requestChunkLoad(Session& s, int blockX, int blockZ) {
    int const cx = blockX >> 4, cz = blockZ >> 4;
    if (s.loadValid && s.loadDim == s.dimid && s.loadCX == cx && s.loadCZ == cz
        && mTickCounter < s.loadPumpTick) {
        return true;   // 刚请求过, 等引擎推进
    }
    s.loadValid    = true;
    s.loadDim      = s.dimid;
    s.loadCX       = cx;
    s.loadCZ       = cz;
    s.loadPumpTick = mTickCounter + RTP_LOAD_PUMP_TICKS;
    return chunkLoadRequest(s.dimid, blockX, blockZ);
}

// 把玩家送到当前随机目标的上方悬停 —— 这是"从未生成过的区块"唯一的生成途径:
// 引擎不会为插件凭空造地（getOrLoadChunk / createNewChunk 都拿不到从未生成过的区块）,
// 但玩家一到, 引擎就会为他的视野把周围区块加载/生成出来（JS 版就是这样做的, 实测可行）。
// 玩家位置在会话结束时会被送回原点（失败/超时/关服都算）。
bool RandomTeleport::ensureHover(Session& s, Player& p) {
    if (s.hoverMoved && p.getPosition().x == (float)(s.randomX + 0.5)
        && p.getPosition().z == (float)(s.randomZ + 0.5)) {
        return true;   // 已经悬停在当前目标上方
    }
    if (!teleportPlayerIfReady(p, Vec3((float)(s.randomX + 0.5), (float)s.hoverY, (float)(s.randomZ + 0.5)),
                               (::DimensionType)s.dimid)) {
        return false;  // 出生流程未完成: 调用方继续等
    }
    if (!s.hoverMoved) {
        RTP_DBG("[RTP][悬停] 玩家 {} 送至 ({}, {}, {}) 上方, 由引擎生成该区块",
                s.playerName, s.randomX, s.hoverY, s.randomZ);
    }
    s.hoverMoved = true;
    return true;
}

// 会话收尾: 处理落点区块的宽限保持（结束/超时/离线/作废/关服统一走这里）。
// 传送成功后不能立刻停手: 那块地若靠本次请求才载入/生成, 立刻撒手会被引擎按常规规则卸载,
// 客户端就收不到区块数据（灰屏）, 所以留 RTP_VIEW_GRACE_TICKS 宽限期, 周期性再请求一次,
// 等玩家自己的视野接管。
void RandomTeleport::cleanupSessionArea(Session& s) {
    if (!s.loadValid) return;
    s.loadValid = false;
    if (s.keepAreaAfterTeleport && s.landingCX >= 0) {
        scheduleKeepAlive(s.loadDim, s.landingCX, s.landingCZ, RTP_VIEW_GRACE_TICKS);
        RTP_DBG("[RTP][加载] 传送成功, 落点区块 ({},{}) 保持 {} tick (dim={})",
                s.landingCX, s.landingCZ, RTP_VIEW_GRACE_TICKS, s.loadDim);
        return;
    }
    RTP_DBG("[RTP][加载] 会话结束 (dim={})", s.loadDim);
}

// 宽限期内保持落点区块被加载
void RandomTeleport::scheduleKeepAlive(int dim, int lcx, int lcz, int delayTicks) {
    mGraceAreas.push_back(GraceArea{dim, lcx, lcz, mTickCounter + delayTicks, mTickCounter, false});
}

void RandomTeleport::processGraceAreas() {
    if (mGraceAreas.empty()) return;
    auto level = ll::service::getLevel();
    for (auto it = mGraceAreas.begin(); it != mGraceAreas.end();) {
        if (mTickCounter < it->removeAtTick) { ++it; continue; }

        // 宽限期内: 周期性再请求一次, 免得落点区块被引擎卸载
        if (mTickCounter < it->removeAtTick) {
            if (mTickCounter >= it->nextPumpTick) {
                if (level) chunkLoadRequest(it->dim, it->lcx << 4, it->lcz << 4);
                it->nextPumpTick = mTickCounter + RTP_LOAD_PUMP_TICKS;
            }
            ++it;
            continue;
        }
        if (!it->removed) {
            it->removed      = true;
            it->removeAtTick = mTickCounter + 20;    // 停止请求后再等 20 tick 做自检
            ++it;
            continue;
        }

        // 自检: 停止请求之后, 落点区块还应该是 Loaded（说明玩家自己的视野已经接管了它）。
        // 若这里读到 Unloaded, 就是"传送后一片灰"的病态情况仍然存在 —— 这条日志是
        // 判断根因是否真的修掉的证据（debug 开启时输出）。
        if (level) {
            auto dim = level->getDimension((::DimensionType)it->dim).lock();
            if (dim) {
                ChunkState st = chunkStateAt(*dim, it->lcx << 4, it->lcz << 4);
                RTP_DBG("[RTP][自检] 停止保持后落点区块({},{}) 状态={}{}",
                        it->lcx, it->lcz, chunkStateName(st),
                        st >= ChunkState::Loaded ? "（玩家视野已接管, 正常）"
                                                 : "（未被接管, 会出现灰屏）");
            }
        }
        it = mGraceAreas.erase(it);
    }
}

// 发起
// 各玩家在各预设上的上次发起时刻（键 = 玩家名 + 预设名）
static std::unordered_map<std::string, int64_t>& rtpCooldowns() {
    static std::unordered_map<std::string, int64_t> m;
    return m;
}

void RandomTeleport::start(Player& player, RtpOptions const& opts) {
    // 冷却: 预设自己设了就用它的, 否则用全局 randomTeleport.cooldownSeconds
    int cooldown = opts.cooldownSeconds > 0 ? opts.cooldownSeconds
                                            : Config::getInstance().randomCooldownSeconds();
    if (cooldown > 0) {
        std::string const key = player.getRealName() + "|" + opts.message;
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
    s->radius     = std::max(1, opts.radius);
    s->usePlayerOrigin = (opts.originMode == "player");
    s->originX = opts.originX;
    s->originZ = opts.originZ;
    s->originPos = player.getPosition();
    s->originDim = s->dimid;
    if (s->usePlayerOrigin) { // 发起那一刻记下原点
        s->originX = std::floor(player.getPosition().x);
        s->originZ = std::floor(player.getPosition().z);
    }
    s->yRange     = getDimYRange(s->dimid);
    s->scanStartY = std::min(getScanStartY(s->dimid), s->yRange.maxY);
    // 悬停高度（"从未生成"支路用）: 主世界 799（远高于建筑上限, 悬停期间不会落进地形）,
    // 其他维度取扫描起点上方 30 格
    s->hoverY = (s->dimid == 0) ? 799 : s->scanStartY + 30;
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
    // 注意: 本函数只算坐标; 若会话已经进入悬停模式（见 ensureHover）, 由调用方负责把玩家搬过去

    // 圆内随机挑点（√r 均匀分布），避开之前失败的落点
    int avoidDist = std::min(640, std::max(128, s.radius / 4));
    std::uniform_real_distribution<double> dist01(0, 1);
    double angle = 0, dist = 0;
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
        if (!tooClose) break;
        if (tries == 11) { angle = dist01(rng()) * kPi * 2; dist = std::sqrt(dist01(rng())) * s.radius; }
    }
    s.randomX = (int)std::round(s.originX + std::cos(angle) * dist);
    s.randomZ = (int)std::round(s.originZ + std::sin(angle) * dist);
    s.triedTargets.push_back({s.randomX, s.randomZ});

    RTP_DBG("[RTP][落点] #{} ({}, {}) 距原点{}格{}", s.triedTargets.size(), s.randomX, s.randomZ,
            (int)std::round(dist), s.triedTargets.size() > 1
                ? "（重随, 剩余名额" + std::to_string(s.reRandomLeft) + "）" : "");

    s.state      = Session::PROBE;
    s.chunkWait  = 0;
    s.expandRing = 0;
    s.expandIdx  = 0;
}

// 完成（成功: 直接传送到安全点 / 失败: 退款提示, 玩家全程原地）
void RandomTeleport::finishTeleport(Session& s, Player& p, bool success, SafePos const* pos) {
    s.finished = true;
    if (success && pos) {
        // 落点所在区块先向引擎请求一次: 磁盘里有就载入内存（玩家落地时客户端立刻能收到区块）,
        // 没有就排队生成; 传送后的宽限期继续周期性保持，直到玩家自己的视野接管。
        requestChunkLoad(s, (int)std::floor(pos->x), (int)std::floor(pos->z));
        s.landingCX = (int)std::floor(pos->x) >> 4;
        s.landingCZ = (int)std::floor(pos->z) >> 4;
        s.keepAreaAfterTeleport = true;   // 落点区块的宽限保持（见 cleanupSessionArea）
        if (!teleportPlayerIfReady(p, Vec3((float)pos->x, (float)pos->y, (float)pos->z),
                                   (::DimensionType)pos->dimid)) {
            // 正常不会走到这（会话推进前已等出生完成）; 真发生说明出生流程异常
            rtpLogger().warn("[RTP] 玩家 {} 出生流程未完成, 本次传送未执行", s.playerName);
        }
        sendActionbar(p, "§a传送成功");
        std::string costTip = (s.cost > 0 && Config::getInstance().economyEnabled())
            ? " §7(花费 §e" + std::to_string(s.cost) + "§7)" : "";
        p.sendMessage(s.message + " §7(" + std::to_string((int)std::floor(pos->x)) + ", " +
                      std::to_string((int)std::floor(pos->y)) + ", " + std::to_string((int)std::floor(pos->z)) + ")" + costTip);
        RTP_DBG("[RTP][结束] {} 成功传送至 ({}, {}, {}) 耗时{}ms/{}tick 落点尝试{}次",
                s.playerName, (int)std::floor(pos->x), (int)std::floor(pos->y), (int)std::floor(pos->z), msSince(s.startedAt), s.totalTicks, s.triedTargets.size());
    } else {
        // 悬停模式下玩家被送上去过, 必须送回原点（否则会留在高空）
        if (s.hoverMoved) {
            teleportPlayerIfReady(p, s.originPos, (::DimensionType)s.originDim);
            s.hoverMoved = false;
        }
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
}

// 单 chunk 判定（三源逐级）: Safe = 找到安全列 / Unsafe = 有数据但整 chunk 无安全列 /
// NoData = 内存与存档都查不到 → 需走生成路径。
RandomTeleport::StepResult RandomTeleport::stepProbe(Session& s, Level& level, Player& p, Dimension& dim) {
    int cx = s.randomX >> 4, cz = s.randomZ >> 4;
    SafePos     pos{};
    std::string reason;
    bool        cheap = false;
    switch (resolveChunk(dim, s.dimid, cx, cz, s.yRange, s.scanStartY,
                         s.dangerSet, s.dangerShortSet, pos, reason, cheap)) {
        case ChunkVerdict::Safe:
            RTP_DBG("[RTP][探测] #{} ({},{}) {} 命中安全列 ({}, {}, {})",
                    s.triedTargets.size(), s.randomX, s.randomZ,
                    cheap ? "落点表" : "内存/直读", (int)pos.x, (int)pos.y, (int)pos.z);
            finishTeleport(s, p, true, &pos);
            return StepResult::Done;
        case ChunkVerdict::Unsafe:
            RTP_DBG("[RTP][探测] #{} ({},{}) {} → 进入扩圈搜索",
                    s.triedTargets.size(), s.randomX, s.randomZ, reason);
            s.state      = Session::EXPAND;
            s.expandRing = 1;
            s.expandIdx  = 0;
            return StepResult::Progress;
        case ChunkVerdict::NoData:
        default:
            // 从未生成过: 引擎不会为插件凭空造地, 得靠玩家自己把这块地"带出来" ——
            // 送到目标上方悬停, 引擎的视野机制就会加载/生成, 下 tick 开始轮询状态。
            if (!chunkInWorldLimit(s.dimid, s.randomX, s.randomZ)) {
                RTP_DBG("[RTP][探测] #{} ({},{}) 超出世界边界, 直接换点",
                        s.triedTargets.size(), s.randomX, s.randomZ);
                if (s.reRandomLeft > 0) { s.reRandomLeft--; pickNewTarget(s); return StepResult::Progress; }
                finishTeleport(s, p, false, nullptr);
                return StepResult::Done;
            }
            RTP_DBG("[RTP][探测] #{} ({},{}) {} → 送玩家到上方悬停, 由引擎生成",
                    s.triedTargets.size(), s.randomX, s.randomZ, reason);
            if (!ensureHover(s, p)) return StepResult::Waiting;   // 出生流程未完成: 下 tick 再试
            s.state = Session::LOAD_CHUNK;
            return StepResult::Waiting; // 生成中, 下 tick 轮询
    }
}

// 等引擎把区块推到就绪, 就绪后转去判定
RandomTeleport::StepResult RandomTeleport::stepLoadChunk(Session& s, Level& level, Player& p, Dimension& dim) {
    s.chunkWait++;
    if (s.chunkWait > RTP_CHUNK_WAIT_TICKS) {
        // 本落点区块 10 秒仍未生成完成：换点重随或放弃
        RTP_DBG("[RTP][等待] #{} ({},{}) 区块{}tick未就绪(最后状态={}), {}",
                s.triedTargets.size(), s.randomX, s.randomZ, RTP_CHUNK_WAIT_TICKS,
                chunkStateName(chunkStateAt(dim, s.randomX, s.randomZ)),
                s.reRandomLeft > 0 ? "换点重随" : "重随名额用尽, 放弃");
        if (s.reRandomLeft > 0) { s.reRandomLeft--; pickNewTarget(s); return StepResult::Progress; }
        finishTeleport(s, p, false, nullptr);
        return StepResult::Done;
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

        // 悬停模式下玩家可能已经跟着换过点（换点重随）: 确保悬停在当前目标上方
        if (s.hoverMoved && !ensureHover(s, p)) return StepResult::Waiting;
        // 顺手向引擎请求一次（已存在的区块能被直接载入; 从未生成的这里拿不到, 靠玩家视野生成）
        requestChunkLoad(s, s.randomX, s.randomZ);
        if (s.chunkWait % 20 == 1) {
            RTP_DBG("[RTP][等待] #{} ({},{}) 第{}tick 状态={}",
                    s.triedTargets.size(), s.randomX, s.randomZ, s.chunkWait,
                    chunkStateName(chunkStateAt(dim, s.randomX, s.randomZ)));
        }
        return StepResult::Waiting; // 等生成完成, 下 tick 再查
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
    int cx = s.randomX >> 4, cz = s.randomZ >> 4;
    if (resolveChunk(dim, s.dimid, cx, cz, s.yRange, s.scanStartY,
                     s.dangerSet, s.dangerShortSet, pos, reason, cheap) == ChunkVerdict::Safe) {
        RTP_DBG("[RTP][落点判定] chunk({},{}) 命中安全列 ({}, {}, {})",
                cx, cz, (int)pos.x, (int)pos.y, (int)pos.z);
        finishTeleport(s, p, true, &pos);
        return StepResult::Done;
    }
    RTP_DBG("[RTP][落点判定] chunk({},{}) 不安全: {} → 进入扩圈搜索", cx, cz, reason);
    s.state      = Session::EXPAND;
    s.expandRing = 1;
    s.expandIdx  = 0;
    return StepResult::Progress;
}

// 扩圈搜索: 一圈一圈往外找安全列, 每 tick 限量
RandomTeleport::StepResult RandomTeleport::stepExpand(Session& s, Level& level, Player& p, Dimension& dim) {
    auto& ring = expandRing(s.expandRing);
    int expensive = 0;  // 本 tick 内存扫描 + 存档直读次数（预算控制）
    int cheapCnt  = 0;  // 本 tick 落点表命中次数（独立、宽得多的预算）
    int pending = 0;    // 扩圈范围内未就绪 chunk 数（等生成, 不耗预算）
    int missed  = 0;    // 区外无数据 chunk 数（跳过）
    int i = s.expandIdx;
    for (; i < (int)ring.size(); i++) {
        if (expensive >= RTP_SCAN_CHUNKS_PER_TICK) break;
        if (cheapCnt >= RTP_TABLE_CHUNKS_PER_TICK) break;
        int ccx = (s.randomX >> 4) + ring[i].dx;
        int ccz = (s.randomZ >> 4) + ring[i].dz;
        if (pending >= 16) break;                   // 单 tick 最多发 16 个生成请求
        // 扩圈范围内但未就绪的区块: 请求引擎加载/生成, 本圈下个 pass 重扫
        if (s.expandRing <= RTP_LOAD_RADIUS_CHUNKS && !isChunkReady(dim, ccx << 4, ccz << 4)) {
            requestChunkLoad(s, ccx << 4, ccz << 4);
            pending++;
            continue;
        }
        // 判定（内部三源逐级: 内存优先 → 落点表 → 存档直读; 查不到 = 未生成的 chunk）
        SafePos     pos{};
        std::string reason;
        bool        cheap = false;
        ChunkVerdict v = resolveChunk(dim, s.dimid, ccx, ccz, s.yRange, s.scanStartY,
                                      s.dangerSet, s.dangerShortSet, pos, reason, cheap);
        if (cheap) cheapCnt++; else expensive++;
        if (v == ChunkVerdict::Safe) {
            RTP_DBG("[RTP][扩圈] r={} chunk({},{}) 命中安全列 ({}, {}, {})",
                    s.expandRing, ccx, ccz, (int)pos.x, (int)pos.y, (int)pos.z);
            finishTeleport(s, p, true, &pos);
            return StepResult::Done;
        }
        if (v == ChunkVerdict::NoData) missed++;
    }
    s.expandIdx = i;

    if (i < (int)ring.size()) return StepResult::Progress;        // 本圈没扫完: 下 tick 继续（预算控制）
    if (pending > 0) {                          // 本圈扫完但区内有 chunk 仍在生成: 等下 tick 重扫本圈
        RTP_DBG("[RTP][扩圈] r={} 圈扫完, {}个区内chunk生成中, 下tick重扫", s.expandRing, pending);
        s.expandIdx = 0;
        return StepResult::Waiting;
    }
    // 本圈彻底扫完
    RTP_DBG("[RTP][扩圈] r={} 圈扫描完毕(未命中, 区外无数据{})", s.expandRing, missed);
    if (s.expandRing < RTP_EXPAND_MAX_RING) {
        s.expandRing++;
        s.expandIdx = 0;
        return StepResult::Progress;
    }
    // 24 圈（384 格）耗尽: 大洋中央/大片未生成区域 → 换点重随
    RTP_DBG("[RTP][扩圈] 全部{}圈耗尽, {}", RTP_EXPAND_MAX_RING,
            s.reRandomLeft > 0 ? "换随机点重随" : "重随名额用尽");
    if (s.reRandomLeft > 0) {
        s.reRandomLeft--;
        pickNewTarget(s);
        return StepResult::Progress;
    }
    finishTeleport(s, p, false, nullptr);
    return StepResult::Done;
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

    // actionbar 节流（约 500ms 一次）
    if (s.titleTick <= 0) { sendActionbar(*p, "§e随机传送中......"); s.titleTick = 10; }
    s.titleTick--;

    // 出生流程未完成不传送（否则引擎会把玩家放回出生点, 客户端一片灰; 立刻 /tpr 即复现）。
    // 上限 RTP_SPAWN_WAIT_TICKS: 超时就当出生流程异常放行, 免得死等。
    if (!isPlayerSpawned(*p) && s.spawnWaitTicks < RTP_SPAWN_WAIT_TICKS) {
        s.spawnWaitTicks++;
        if (s.spawnWaitTicks == 1) {
            RTP_DBG("[RTP][等待] {} 出生流程未完成, 暂缓传送", s.playerName);
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

// 调度（多会话轮流推进, 单会话限步数）
void RandomTeleport::tick() {
    mTickCounter++;
    // 首个 tick: 清理旧版本用 /tickingarea 生成、残留在存档里的常加载区域
    static bool sPurged = false;
    if (!sPurged) {
        sPurged = true;
        auto level = ll::service::getLevel();
        if (level) purgeLegacyTickingAreas(*level);
    }
    processGraceAreas();   // 传送成功后保留的视野: 宽限期到就释放
    // 时间预算: 按会话数平分, 每个会话至少允许推进一步（否则会被饿死）。
    // 外层还有一道总闸: 会话多的时候"平分"会让总时间超过预算, 所以总耗时到上限就停。
    auto const tickStart    = std::chrono::steady_clock::now();
    auto const tickLimit    = tickStart + std::chrono::milliseconds(RTP_TICK_BUDGET_MS);
    int const  sessionCount = mSessions.empty() ? 1 : (int)mSessions.size();
    auto const slice        = std::chrono::milliseconds(RTP_TICK_BUDGET_MS / sessionCount + 1);

    for (auto it = mSessions.begin(); it != mSessions.end();) {
        auto& s = *it;
        // 本 tick 已经用满预算: 剩下的会话留到下个 tick（至少让第一个会话跑完一步）
        if (it != mSessions.begin() && std::chrono::steady_clock::now() > tickLimit) break;
        s->deadline = tickStart + slice;
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
}

// 停止全部会话（悬停中的玩家送回原点 —— 否则关服时那个高空位置会被写进存档）
void RandomTeleport::stopAll() {
    if (auto level = ll::service::getLevel()) {
        for (auto& s : mSessions) {
            if (!s->hoverMoved) continue;
            if (Player* p = level->getPlayer(s->playerName)) {
                teleportPlayerIfReady(*p, s->originPos, (::DimensionType)s->originDim);
            }
        }
    }
    mSessions.clear();
    mGraceAreas.clear();
}

} // namespace mtps
