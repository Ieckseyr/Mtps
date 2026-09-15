#pragma once
// BedrockLevelReader: 直接解析存档里的 .ldb (SSTable), 不经过 leveldb 库 ——
// BDS 占着 LOCK、MANIFEST/WAL 是 Mojang v2 记录格式、压缩类型也非标准, DB::Open 走不通。
// 只索引 subchunk key(0x2F); 新版 key 带 8 字节写入序号后缀, 查询按前缀取最新版本。
// open()/close() 需独占; 之前后 scanChunkSurface / getBlockName / scanWorldBounds 可并发。
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <memory>
#include <utility>

class BedrockLevelReader {
public:
    // 一个 chunk 列（16x16）的表面扫描结果
    struct ChunkSurface {
        std::string block[256];      // 短方块名（无 minecraft: 前缀），空串=无方块/air
        int16_t     height[256];     // 表面方块 y，INT16_MIN=无方块
        uint16_t    waterDepth[256]; // 上方水层数

        ChunkSurface() {
            for (int i = 0; i < 256; i++) {
                block[i].clear();
                height[i] = INT16_MIN;
                waterDepth[i] = 0;
            }
        }
    };

    explicit BedrockLevelReader(std::filesystem::path dbPath);
    ~BedrockLevelReader();

    // 并行扫描目录内全部 .ldb 建立索引（大存档可能需要十几秒）
    bool open();
    void close();
    bool isOpen() const { return mOpened; }

    // 扫描一个 chunk 的表面方块（从顶部向下找第一个非空气方块）
    // dim: 0=overworld, 1=nether, 2=end
    ChunkSurface scanChunkSurface(int cx, int cz, int dim);

    // 落点预计算表（RTP 快路径）: 后台一次性算好每 chunk 的安全落点, 查询退化成内存二分。
    // 表内已固化当时的 dangerBlocks 判定 —— 改 dangerBlocks 需重启才对该层生效。
    struct Landing {
        int32_t cx{0}, cz{0};
        int32_t y{0};            // 安全落点站立 Y（state == LAND_SAFE 时有效）
        int8_t  lx{-1}, lz{-1};  // chunk 内列坐标（0..15）
        uint8_t state{0};
    };
    static constexpr uint8_t LAND_SAFE    = 1;  // 有安全列
    static constexpr uint8_t LAND_NO_SAFE = 2;  // 有存档数据, 但整 chunk 无安全列

    // 后台预计算（要读盘, 必须放后台线程; 可用 requestCancel 中断）
    // dangerShort: 危险方块短名（无 minecraft: 前缀）; onProgress 可空
    bool buildLandings(std::vector<std::string> const& dangerShort,
                       std::function<void(size_t, size_t)> const& onProgress);

    // 查表: 返回 false = 存档里没有该 chunk（从未生成, 或表尚未覆盖）
    bool lookupLanding(int cx, int cz, int dim, Landing& out) const;
    size_t landingCount() const;

    // 该维度表里的 chunk 数（0 = 这一维度存档里什么都没生成, 扩圈扫了也全是无数据）
    size_t landingCountForDim(int dim) const;

    // 反复随机抽表里的一项, 取第一个"在半径内且已有安全落点"的 chunk（拒绝采样, 零 IO）。
    // seed 由调用方给（每次调用换个随机数）; tries 用尽或表未就绪/为空返回 false。
    bool pickSafeLandingInRange(int originBX, int originBZ, int radiusBlocks, int dim,
                                uint64_t seed, Landing& out, int tries = 128) const;
    bool   landingsReady() const { return mLandingsReady.load(std::memory_order_acquire); }

    // 请求中断（open 的并行扫描 / buildLandings 都会尽快退出）
    void requestCancel() { mCancel.store(true, std::memory_order_release); }
    bool cancelled() const { return mCancel.load(std::memory_order_acquire); }

    // 获取指定 subchunk 的某个方块的短名（无 minecraft: 前缀）
    // lx/lz: 0..15（chunk 内坐标）, y: 世界绝对 Y 坐标
    std::string getBlockName(int cx, int cz, int dim, int lx, int lz, int y);

    // 基于已建索引收集指定维度所有已生成 chunk 的边界（用于决定渲染范围）
    struct WorldBounds {
        int minCX = 0, maxCX = 0, minCZ = 0, maxCZ = 0;
        size_t chunkCount = 0;   // 该维度 subchunk key 数量
        bool valid = false;
    };
    WorldBounds scanWorldBounds(int dim);

    // 列出指定维度所有存在 subchunk key 的 chunk 坐标（升序去重）。
    // 渲染关键: 边界框内 ~99% 的 region 是空的, 逐 chunk 探测太浪费（百万 chunk ≈ 8MB）。
    std::vector<std::pair<int32_t, int32_t>> listChunks(int dim);

    // 诊断信息
    size_t indexSize() const { return mIndex.size(); }
    bool overworldKeyHasDim() const { return mOverworldKeyHasDim; }
    // open() 后调用：块压缩类型分布 / key 长度分布 / 解压失败数（用于排查索引问题）
    std::string diagInfo() const;

private:
    // 标准 subchunk key 最长 14 字节（cx4+cz4+dim4+tag1+subY1）
    static constexpr size_t MAX_KEY_LEN = 14;
    // 新版 key 的尾部后缀长度
    static constexpr size_t KEY_SUFFIX_LEN = 8;

    // 紧凑索引（两级：key → block 表下标）
    // KeyEntry 20B：1550 万 subchunk key ≈ 300MB → 只索引 subchunk 约 190MB
    struct KeyEntry {
        uint8_t  key[MAX_KEY_LEN] = {0}; // 标准 key（已剥离后缀）
        uint8_t  keyLen = 0;
        uint8_t  _pad = 0;
        uint32_t blockIdx = 0;           // mBlocks 下标
    };
    struct BlockEntry {
        int32_t  fileIdx = -1;
        uint32_t dataSize = 0;
        uint64_t blockOffset = 0;
    };
    static int keyCompare(const KeyEntry& e, const uint8_t* k, size_t klen);

    // .ldb 文件（句柄保持打开，FILE_SHARE_DELETE 保证 BDS 可删）
    struct LdbFile {
        void*   handle = nullptr;  // HANDLE (win32)
        uint64_t fileSize = 0;
        // 主句柄被多个渲染 worker 共享：同步句柄的 SetFilePointerEx+ReadFile
        // 非原子（文件指针被并发覆盖会读错位数据）→ loadBlock 的 seek+read
        // 必须串行（open() 并行扫描阶段每线程独立句柄，不受此影响）
        mutable std::mutex ioMutex;

        // std::mutex 不可移动 → 隐式删除移动构造会使 vector<LdbFile>::resize 编译失败，
        // 手写移动：句柄转移，mutex 全新构造（扩容搬迁发生在 open() 单线程阶段，无并发持锁）
        LdbFile() = default;
        LdbFile(LdbFile&& other) noexcept
        : handle(other.handle), fileSize(other.fileSize) {
            other.handle  = nullptr;
            other.fileSize = 0;
        }
        LdbFile& operator=(LdbFile&& other) noexcept {
            if (this != &other) {
                handle   = other.handle;
                fileSize = other.fileSize;
                other.handle   = nullptr;
                other.fileSize = 0;
            }
            return *this;
        }
        LdbFile(LdbFile const&)            = delete;
        LdbFile& operator=(LdbFile const&) = delete;
    };

    // block 解压缓存（FIFO 淘汰, key = blockIdx; shared_ptr 保证并发淘汰时的生命周期）。
    // 16384 块 ≈ 64MB: 命中率是渲染期 IO 的关键, 太小会反复读盘 + ZSTD 解压。
    std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> mBlockCache;
    std::deque<uint32_t> mBlockCacheOrder;
    std::mutex mBlockCacheMutex;
    static constexpr size_t MAX_BLOCK_CACHE = 16384;

    // 单文件扫描结果（并行收集后合并；扫描线程只写自己的 ScanResult，无共享）
    struct ScanResult {
        std::vector<KeyEntry> keys;
        std::vector<BlockEntry> blocks;
        uint64_t blocksRead = 0;
        uint64_t decompFail = 0;
        uint64_t typeHist[8] = {0};
        uint64_t subchunkNewFmt = 0; // 带后缀的新格式 subchunk key 数
        uint64_t subchunkOldFmt = 0; // 老格式 subchunk key 数
    };
    bool scanFile(int fileIdx, const std::wstring& path, void* fileHandle, uint64_t fileSize,
                  ScanResult& out);

    std::shared_ptr<const std::vector<uint8_t>> loadBlock(uint32_t blockIdx);
    // 前缀匹配查找：在解压后的 block 中找以 key 开头的 entry，
    // 同前缀多版本（新版 key 后缀不同）取最后一条（后缀字节序最大 = 写入序号最大 = 最新）
    bool findInBlock(const uint8_t* data, size_t size,
                     const uint8_t* key, size_t keyLen, std::string& outValue);
    // 指针版（零拷贝）：调用方须保证 block 缓存在使用期间存活（持有 shared_ptr 即可）
    bool findInBlockPtr(const uint8_t* data, size_t size,
                        const uint8_t* key, size_t keyLen,
                        const uint8_t*& outPtr, size_t& outLen);

    // 查询一个 subchunk 的原始值：二分索引 → 读 block → 前缀匹配（线程安全）
    bool getSubchunkValue(const uint8_t* key, size_t keyLen, std::string& outValue);

    // 批量收集一个 chunk 的全部 subchunk 原始值（渲染路径专用）: 一次二分定位 + 顺序遍历,
    // 每个 data block 只加载一次; fn(subIdx, valuePtr, valueLen) 按 subIdx 升序回调。
    template <typename Fn>
    bool collectChunkSubchunks(int cx, int cz, int dim, Fn&& fn);

    static bool readFullyAt(void* handle, uint64_t offset, void* buf, size_t n);

    // 遍历解压后 block 的所有 entries：fn(key, valuePtr, valueLen)
    template <typename Fn>
    static bool forEachEntryInBlock(const uint8_t* data, size_t size, Fn&& fn);

    // 依赖 mOverworldKeyHasDim（open() 时检测的主世界 key 格式）
    void buildSubchunkKey(uint8_t* out, uint8_t& outLen,
                          int cx, int cz, int dim, int8_t subchunkIndex) const;

    // subchunk 解析（palette + 紧凑索引）
    struct SubchunkData {
        std::vector<std::string> palette;      // layer 0 方块名（短名）
        std::vector<uint16_t> blockIndices;    // 4096 个，YZX 顺序
        std::vector<std::string> liquidPalette; // layer 1 液体名（若有）
        std::vector<uint16_t> liquidIndices;
        bool valid = false;
    };
    SubchunkData parseSubchunk(const std::string& value);
    SubchunkData parseSubchunk(const uint8_t* data, size_t size);

    // Bedrock 小端 NBT 解析器（仅提取 compound 中的 name 字符串）
    struct NBTReader {
        const uint8_t* data = nullptr;
        size_t size = 0;
        size_t pos = 0;

        bool readByte(int8_t& v);
        bool readShort(int16_t& v);
        bool readInt(int32_t& v);
        bool readString(std::string& s); // 2字节LE长度 + UTF8
        bool skipTag(uint8_t type);
        bool readRootCompound(std::string& outName);
    };

    // PalettedStorage 解析（一个 layer）
    struct PaletteStorage {
        std::vector<uint32_t> words;
        int bitsPerIndex = 0;
        int blocksPerWord = 0;
        int wordCount = 0;
        std::vector<std::string> palette; // 方块名（短名，已 strip minecraft:）
    };
    bool parsePaletteStorage(const uint8_t* data, size_t size, size_t& offset, PaletteStorage& out);
    static uint32_t getPaletteIndex(const PaletteStorage& ps, int x, int y, int z);

    // subchunk 解析结果缓存（低频 API 用；渲染路径不走缓存）
    std::unordered_map<std::string, std::shared_ptr<SubchunkData>> mSubchunkCache;
    std::mutex mSubchunkCacheMutex;
    static constexpr size_t MAX_SUBCHUNK_CACHE = 2048;

    std::filesystem::path mDbPath;
    bool mOpened = false;
    std::vector<KeyEntry> mIndex;    // open() 后只读（已排序去重）
    std::vector<BlockEntry> mBlocks; // open() 后只读
    std::vector<LdbFile> mFiles;     // open() 后只读（主句柄）
    // 主世界 subchunk key 是否带 dim 字段：
    //   false = 10B：cx+cz+0x2F+subY（老格式）
    //   true  = 14B：cx+cz+dim(0)+0x2F+subY（部分 BDS 版本/新格式）
    bool mOverworldKeyHasDim = false;

    // 落点预计算表（buildLandings 填充; 完成后只读）
    std::vector<Landing> mLandings[3];        // 按 (cx, cz) 升序, 每维一张
    std::atomic<bool>    mLandingsReady{false};
    std::atomic<bool>    mCancel{false};

    // subchunk 逐列探测结果（surface-only 解码: 不物化 4096 索引、不构造 palette 字符串副本）
    struct ColProbe {
        int16_t  topSolidY{-1};  // 本 subchunk 内最高实体方块的 localY; -1 = 本 subchunk 无实体方块
        uint16_t waterAbove{0};  // 该实体方块之上的水层数（无实体方块时 = 本 subchunk 全部水层）
        uint8_t  verdict{0};     // 该实体方块的判定: 1=可用 2=危险方块 3=空名(视为无列)
    };
    // 解析一个 subchunk 并填充 256 列的 ColProbe（与 scanChunkSurface 的逐列规则一致）
    void decodeSubchunkColumns(const uint8_t* data, size_t size,
                               std::unordered_set<std::string> const& dangerShort,
                               ColProbe (&out)[256]);

    // open() 扫描统计（诊断用；open() 完成后只读）
    struct ScanStats {
        uint64_t filesTotal = 0;   // .ldb 文件总数
        uint64_t filesParsed = 0;  // footer/index 解析成功的文件数
        uint64_t blocksRead = 0;   // 处理的 data block 总数
        uint64_t decompFail = 0;   // 解压失败次数
        uint64_t typeHist[8] = {0};// 块压缩类型计数 0..7
        uint64_t keyLenHist[40] = {0}; // 剥离前 key 长度直方图（>=39 归入 39）
        uint64_t subchunkNewFmt = 0; // 带后缀的新格式 subchunk key 数
        uint64_t subchunkOldFmt = 0; // 老格式 subchunk key 数
    };
    ScanStats mStats;
};
