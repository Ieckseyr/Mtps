#pragma once
// ArchiveScanner: RTP 存档直读的后台封装（.ldb 索引 → 落点预计算, 都在后台线程）。
// 就绪前 RTP 走原有同步路径, 所以预热期功能不受影响, 只是逐步变快。
// 只能查到"已生成且已落盘"的区块; 运行中未保存的由 RTP 的内存层覆盖。
#include "BedrockLevelReader.h"

#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>

namespace mtps {

class ArchiveScanner {
public:
    static ArchiveScanner& getInstance();

    // enable 时调用: 启动后台线程建索引 + 建落点表（幂等）
    void startAsync();
    // disable 时调用: 中断并停线程关 reader（关服时 RTP stopAll 后调用）
    void shutdown();

    // 索引是否已建立（open 完成且成功）
    bool ready() const { return mReady.load(std::memory_order_acquire); }
    // 落点预计算表是否已就绪（就绪后 RTP 查询零 IO）
    bool landingsReady() const;
    size_t landingCount() const;

    // 查一个 chunk 的表面数据（256 列: 方块名/高度/水深）; 未就绪或无数据返回空结果
    BedrockLevelReader::ChunkSurface scanChunk(int cx, int cz, int dim);

    // 落点表查询（每 chunk 一次内存二分）; false = 该 chunk 不在表中（走同步兜底）
    bool lookupLanding(int cx, int cz, int dim, BedrockLevelReader::Landing& out);

    // 诊断信息（open 耗时/索引规模等, 日志用）
    std::string diagInfo() const;

private:
    ArchiveScanner() = default;
    ~ArchiveScanner() { shutdown(); }
    ArchiveScanner(ArchiveScanner const&)            = delete;
    ArchiveScanner& operator=(ArchiveScanner const&) = delete;

    // db 目录定位: server.properties level-name → worlds/<名>/db
    static std::string resolveDbPath();

    std::unique_ptr<BedrockLevelReader> mReader;
    std::unique_ptr<std::thread>        mOpenThread;
    std::mutex                          mMutex;   // shutdown 与后台线程互斥
    std::atomic<bool>                   mReady{false};
    std::atomic<bool>                   mStopping{false};
};

} // namespace mtps
