// ArchiveScanner.cpp - RTP 存档直读封装实现
// dbPath 定位: 可执行文件目录 / server.properties 的 level-name → worlds/<name>/db
// （与 ZXPanel 地图渲染同一套逻辑, 已验证）。
#include "ArchiveScanner.h"
#include "Config.h"

#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>

#include <chrono>
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

void ArchiveScanner::startAsync() {
    if (mOpenThread) return; // 已启动（幂等）

    mStopping.store(false, std::memory_order_release);
    auto dbPath = resolveDbPath();
    if (dbPath.empty()) {
        scannerLogger().warn("[RTP][存档] 未找到世界 db 目录, 存档直读不可用（RTP 回退生成路径）");
        return;
    }

    // reader 在派生线程之前创建: shutdown 才能拿到它并请求中断
    // （否则关服/重载会被 join 卡住, 必须等整份索引扫完）
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mReader = std::make_unique<BedrockLevelReader>(dbPath);
    }

    mOpenThread = std::make_unique<std::thread>([this]() {
        auto reader = mReader.get();
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
                });
        } catch (...) {
            built = false;
        }
        auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t2).count();
        if (built) {
            scannerLogger().info("[RTP][落点表] 预计算完成({}ms): {} 个 chunk 已可零 IO 查询"
                                 "（dangerBlocks 规则已固化, 改配置需重启）",
                                 ms2, reader->landingCount());
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
