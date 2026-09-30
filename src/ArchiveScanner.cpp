// ArchiveScanner.cpp - RTP 存档直读封装实现
// dbPath 定位: 可执行文件目录 / server.properties 的 level-name → worlds/<name>/db
// （与 ZXPanel 地图渲染同一套逻辑, 已验证）。
#include "ArchiveScanner.h"
#include "Config.h"
#include "RandomTeleportInternal.h"   // RTP_LANDING_DEFER_SECONDS

#include <ll/api/mod/NativeMod.h>
#include <ll/api/service/Bedrock.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <ll/api/io/Logger.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>

namespace mtps {

static ll::io::Logger& scannerLogger() {
    return ll::mod::NativeMod::current()->getLogger();
}

ArchiveScanner& ArchiveScanner::getInstance() {
    static ArchiveScanner instance;
    return instance;
}

std::string ArchiveScanner::resolveDbPath() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto serverDir = fs::current_path(ec);
    if (ec) return {};

    std::string levelName = "Bedrock level";
    {
        std::ifstream props(serverDir / "server.properties");
        std::string line;
        while (std::getline(props, line)) {
            if (line.rfind("level-name=", 0) == 0) {
                levelName = line.substr(11);
                while (!levelName.empty()
                       && (levelName.back() == '\r' || levelName.back() == '\n')) {
                    levelName.pop_back();
                }
                break;
            }
        }
    }
    auto dbPath = serverDir / "worlds" / levelName / "db";
    if (!fs::is_directory(dbPath, ec)) return {};
    return dbPath.string();
}

// db 目录下所有 .ldb 的 (名字, 大小, 修改时间) 哈希: 世界没变过 → 落点表缓存依然有效
std::string ArchiveScanner::ldbFingerprint() {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto const      dbPath = resolveDbPath();
    if (dbPath.empty()) return {};
    std::vector<std::string> parts;
    for (auto const& e : fs::directory_iterator(dbPath, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        auto const name = e.path().filename().string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".ldb") != 0) continue;
        std::error_code e2;
        parts.push_back(name + ":" + std::to_string((long long)e.file_size(e2)) + ":"
                        + std::to_string((long long)fs::last_write_time(e.path(), e2).time_since_epoch().count()));
    }
    if (parts.empty()) return {};
    std::sort(parts.begin(), parts.end());
    std::string all;
    for (auto& p : parts) { all += p; all += '|'; }
    return std::to_string(std::hash<std::string>{}(all));
}

std::string ArchiveScanner::cacheFilePath() {
    return (Config::dataDir() / "landings.cache").string();
}

// 缓存命中就用它: 只建 reader 对象（不开索引）, 把表直接读回来 —— 秒级, 不碰磁盘扫描
bool ArchiveScanner::tryLoadLandingsCache(std::string const& dbPath) {
    auto const cache = cacheFilePath();
    std::error_code ec;
    if (!std::filesystem::exists(cache, ec)) return false;

    auto const fp = ldbFingerprint();
    if (fp.empty()) return false;

    std::ifstream cf(cache + ".meta");
    if (!cf.is_open()) return false;
    std::string cachedFp, cachedDb, cachedMax, cachedRules;
    std::getline(cf, cachedFp);
    std::getline(cf, cachedDb);
    std::getline(cf, cachedMax);
    std::getline(cf, cachedRules);
    // 上限也一起比（改了 maxLandings 就该重算）；规则版本也比——取面规则变了（比如"树冠不算
    // 地表"），表里的落点就都过时了，光看 .ldb 指纹发现不了。老 meta 没有第 4 行会读到空串，
    // 与当前版本不等 ⇒ 自动重算，正好是要的。
    if (cachedFp != fp || cachedDb != dbPath
        || cachedMax != std::to_string(Config::getInstance().landingPrecomputeMax())
        || cachedRules != kLandingRulesVersion) {
        return false;
    }

    auto reader = std::make_unique<BedrockLevelReader>(dbPath);
    if (!reader->loadLandingsCache(cache)) return false;

    {
        std::lock_guard<std::mutex> lk(mMutex);
        mReader = std::move(reader);
    }
    mReady.store(true, std::memory_order_release);   // 表已就绪 ⇒ 视为可用（走表, 不走同步直读）
    scannerLogger().info("[RTP][落点表] 缓存命中: {} 个 chunk（跳过重算, 世界改动后会自动重建）",
                         mReader ? mReader->landingCount() : 0);
    return true;
}

void ArchiveScanner::startAsync() {
    if (mOpenThread) return; // 已启动（幂等）

    // 配置关掉预计算: 完全不碰存档（不建索引、不扫 .ldb、不读缓存）, RTP 走内存/生成路径
    if (!Config::getInstance().landingPrecomputeEnabled()) {
        scannerLogger().info("[RTP][落点表] 预计算已关闭（randomTeleport.precompute.enabled=false）, "
                             "随机传送走内存/区块生成路径");
        return;
    }

    mStopping.store(false, std::memory_order_release);
    auto dbPath = resolveDbPath();
    if (dbPath.empty()) {
        scannerLogger().warn("[RTP][存档] 未找到世界 db 目录, 存档直读不可用（RTP 回退生成路径）");
        return;
    }

    // ① 先试缓存: 世界没变就直接用（开服阶段只读几 MB, 不抢磁盘、不重算）
    if (tryLoadLandingsCache(dbPath)) return;

    // reader 在派生线程之前创建: shutdown 才能拿到它并请求中断
    // （否则关服/重载会被 join 卡住, 必须等整份索引扫完）
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mReader = std::make_unique<BedrockLevelReader>(dbPath);
    }

    mOpenThread = std::make_unique<std::thread>([this, dbPath]() {
        // 推迟到开服后 45 秒再重算: 整轮 .ldb 扫描 + 逐 chunk 解码会占满磁盘, 和开服抢资源
        // （这段纯 sleep, 每 100ms 看一眼是否要停, 关服不会卡在 join 上）
        for (int i = 0; i < RTP_LANDING_DEFER_SECONDS * 10; i++) {
            if (mStopping.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (mStopping.load(std::memory_order_acquire)) return;

        // 等没人在线再做。重建要解压整份 .ldb（实测 CPU 满载 3 秒以上），和玩家传送撞上时会把
        // MSPT 顶到 100ms+，而这活什么时候做都行：有人在就每 5 秒看一眼、最多等 30 分钟；
        // 到期还没空服就按限流模式照做（见 BedrockLevelReader::open 里的并行度说明）。
        {
            bool waitedLogged = false;
            auto const waitStart = std::chrono::steady_clock::now();
            while (!mStopping.load(std::memory_order_acquire)) {
                bool anyone = false;
                if (auto lvl = ll::service::getLevel()) {
                    try {
                        // 只看"有没有人在线"，找到第一个就停（工程里数在线玩家都走 forEachPlayer，
                        // getPlayerList() 会连离线玩家一起带出来）
                        lvl->forEachPlayer([&](Player&) -> bool { anyone = true; return false; });
                    } catch (...) {}
                }
                if (!anyone) break;
                auto const waitedS = std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::steady_clock::now() - waitStart)
                                         .count();
                if (waitedS >= 1800) {   // 30 分钟兜底: 一直有人也得重建
                    scannerLogger().info("[RTP][落点表] 等空服已 {} 分钟仍有玩家在线, "
                                         "按限流模式开始重建（并行度 2 + 逐块让出 CPU）", waitedS / 60);
                    break;
                }
                if (!waitedLogged) {
                    waitedLogged = true;
                    scannerLogger().info("[RTP][落点表] 有玩家在线, 重建推迟到空服再做"
                                         "（落点表先用旧缓存, 不影响传送, 最多等 30 分钟）");
                }
                for (int i = 0; i < 50 && !mStopping.load(std::memory_order_acquire); i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        if (mStopping.load(std::memory_order_acquire)) return;
        scannerLogger().info("[RTP][落点表] 开始重建落点表（世界改动后一次性重算, 已限流）");

        auto reader = mReader.get();
        if (reader == nullptr) return;
        auto start  = std::chrono::steady_clock::now();
        bool ok     = false;
        try {
            ok = reader->open();
        } catch (...) {
            ok = false;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (mStopping.load(std::memory_order_acquire)) return; // shutdown 已抢先
        if (!ok) {
            scannerLogger().warn("[RTP][存档] .ldb 索引建立失败({}ms), RTP 回退生成路径", ms);
            return;
        }
        mReady.store(true, std::memory_order_release);
        scannerLogger().info(
            "[RTP][存档] .ldb 索引建立成功({}ms): {} 条 key, 诊断: {}",
            ms, reader->indexSize(), reader->diagInfo());

        // 第二阶段: 落点预计算（后台继续跑, 就绪前 RTP 走同步兜底）
        auto& cfg = Config::getInstance();
        auto const& dangerShort = cfg.dangerShortBlocks();
        auto t2 = std::chrono::steady_clock::now();
        bool built = false;
        try {
            built = reader->buildLandings(
                dangerShort,
                [start](size_t done, size_t total) {
                    if (total > 0 && (done == total || (done & 0xFFFF) == 0)) {
                        scannerLogger().info("[RTP][落点表] 预计算进度 {}/{} chunk（已耗时 {}ms）",
                                             done, total,
                                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - start).count());
                    }
                },
                (size_t)Config::getInstance().landingPrecomputeMax());
        } catch (...) {
            built = false;
        }
        auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t2).count();
        if (built) {
            scannerLogger().info("[RTP][落点表] 预计算完成({}ms): {} 个 chunk 已可零 IO 查询{}"
                                 "（dangerBlocks 规则已固化, 改配置需重启）",
                                 ms2, reader->landingCount(),
                                 reader->landingsCapped()
                                     ? "（已到上限 randomTeleport.precompute.maxLandings, 其余走内存/生成）"
                                     : "");
            // 存盘: 下次开服只要世界没改动就直接读它, 不用再算一遍
            try {
                auto const cache = cacheFilePath();
                std::ofstream cf(cache + ".meta", std::ios::trunc);
                cf << ldbFingerprint() << std::endl;
                cf << dbPath << std::endl;
                cf << Config::getInstance().landingPrecomputeMax() << std::endl;
                cf << kLandingRulesVersion << std::endl;   // 取面规则版本（变了就重算）
                if (reader->saveLandingsCache(cache)) {
                    scannerLogger().info("[RTP][落点表] 已存盘: {}", cache);
                }
            } catch (...) {
            }
        } else if (!mStopping.load(std::memory_order_acquire)) {
            scannerLogger().warn("[RTP][落点表] 预计算未完成({}ms), 相关 chunk 走同步直读兜底", ms2);
        }
    });
}

void ArchiveScanner::shutdown() {
    mStopping.store(true, std::memory_order_release);
    {
        // 请求中断: 索引扫描与落点预计算都会尽快退出, join 不再等整个阶段跑完
        std::lock_guard<std::mutex> lk(mMutex);
        if (mReader) mReader->requestCancel();
    }
    if (mOpenThread && mOpenThread->joinable()) {
        mOpenThread->join();
    }
    mOpenThread.reset();
    std::lock_guard<std::mutex> lk(mMutex);
    if (mReader) mReader->close();
    mReader.reset();
    mReady.store(false, std::memory_order_release);
}

bool ArchiveScanner::landingsReady() const {
    return ready() && mReader && mReader->landingsReady();
}

size_t ArchiveScanner::landingCount() const {
    return (ready() && mReader) ? mReader->landingCount() : 0;
}

bool ArchiveScanner::lookupLanding(int cx, int cz, int dim, BedrockLevelReader::Landing& out) {
    if (!ready()) return false;
    // 与 scanChunk 相同的生命周期约定: ready 后 mReader 只增不改,
    // 关服序列 RTP stopAll(不再 tick) → shutdown, 不存在并发销毁窗口
    std::lock_guard<std::mutex> lk(mMutex);
    if (!mReader) return false;
    try {
        return mReader->lookupLanding(cx, cz, dim, out);
    } catch (...) {
        return false;
    }
}

size_t ArchiveScanner::landingCountForDim(int dim) const {
    return (ready() && mReader) ? mReader->landingCountForDim(dim) : 0;
}

std::vector<BedrockLevelReader::Landing> ArchiveScanner::snapshotLandings(int dim) {
    if (!ready() || !mReader) return {};
    std::lock_guard<std::mutex> lk(mMutex);
    if (!mReader) return {};
    try {
        return mReader->landings(dim);   // 拷贝
    } catch (...) {
        return {};
    }
}

bool ArchiveScanner::pickSafeLandingInRange(int originBX, int originBZ, int radiusBlocks, int dim,
                                            uint64_t seed, BedrockLevelReader::Landing& out,
                                            int tries) {
    if (!ready()) return false;
    std::lock_guard<std::mutex> lk(mMutex);
    if (!mReader) return false;
    try {
        return mReader->pickSafeLandingInRange(originBX, originBZ, radiusBlocks, dim, seed, out, tries);
    } catch (...) {
        return false;
    }
}

BedrockLevelReader::ChunkSurface ArchiveScanner::scanChunk(int cx, int cz, int dim) {
    if (!ready()) return {};
    // ready 后 mReader 只增不改（shutdown 前唯一写入点）, 此处无锁读安全;
    // 关服序列: RTP stopAll(不再 tick) → shutdown, 不存在并发销毁窗口
    std::lock_guard<std::mutex> lk(mMutex);
    if (!mReader) return {};
    try {
        return mReader->scanChunkSurface(cx, cz, dim);
    } catch (...) {
        return {};
    }
}

std::string ArchiveScanner::diagInfo() const {
    if (!ready()) return "not-ready";
    return mReader ? mReader->diagInfo() : "not-ready";
}

} // namespace mtps
