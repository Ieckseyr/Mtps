// LandingScan.cpp - 落点预计算（RTP 快路径）
// 索引就绪后在后台一次性算出"每 chunk 一个安全落点"（16 字节/chunk）, 查询退化为一次内存
// 二分: 零 IO、零解压、零分配。逐列规则与 scanChunkSurface + findSafeInSurface 保持一致,
// 保证同一坐标两条路径同解。
#include "BedrockLevelReader.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <unordered_set>

namespace {

inline uint32_t rdLE32_(uint8_t const* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

} // namespace

// 解析一个 subchunk 的 256 列探测结果
// 与 parseSubchunk 的区别: 不物化 4096 项索引数组、不拷贝 palette 字符串、
//   不构造 SubchunkData（原路径每 subchunk 分配 ~16KB, 每 chunk 上百 KB）
void BedrockLevelReader::decodeSubchunkColumns(const uint8_t* data, size_t size,
                                               std::unordered_set<std::string> const& dangerShort,
                                               ColProbe (&out)[256]) {
    for (int i = 0; i < 256; i++) out[i] = ColProbe{};
    if (!data || size == 0) return;

    size_t offset = 0;
    uint8_t version = data[offset++];
    int numStorages = 1;
    if (version == 8 || version == 9) {
        if (offset >= size) return;
        numStorages = data[offset++];
        if (version == 9) {
            if (offset >= size) return;
            offset++;   // y_index（key 中已有, 跳过）
        }
    } else {
        return;         // 只支持 8/9（与 parseSubchunk 一致）
    }
    if (numStorages < 1 || numStorages > 8) return;

    // layer 0 = 地形, layer 1 = 液体（其余 storage 解析后丢弃, 但解析失败要整体放弃）
    PaletteStorage layer0, layer1;
    bool hasLayer1 = false;
    for (int s = 0; s < numStorages; s++) {
        if (s == 0) {
            if (!parsePaletteStorage(data, size, offset, layer0)) return;
        } else if (s == 1) {
            if (!parsePaletteStorage(data, size, offset, layer1)) return;
            hasLayer1 = true;
        } else {
            PaletteStorage skip;
            if (!parsePaletteStorage(data, size, offset, skip)) return;
        }
    }

    // 线程本地复用缓冲（避免每 subchunk 堆分配）
    thread_local std::vector<uint8_t>  tVerdict;
    thread_local std::vector<uint64_t> tLiqBits;

    // palette 逐项判定: 1=可用 2=危险 3=空名（视为无列, 与 findSafeInSurface 的 name.empty() 一致）
    tVerdict.assign(layer0.palette.size(), 0);
    uint16_t airIdx = 0xFFFF, waterIdx = 0xFFFF, flowIdx = 0xFFFF;
    for (size_t i = 0; i < layer0.palette.size(); i++) {
        std::string const& n = layer0.palette[i];
        if (n == "air" || n.empty()) airIdx = (uint16_t)i;
        else if (n == "water") waterIdx = (uint16_t)i;
        else if (n == "flowing_water") flowIdx = (uint16_t)i;
        tVerdict[i] = n.empty() ? 3 : (dangerShort.count(n) ? 2 : 1);
    }

    // 液体层"是水"位图（4096 位, 按紧凑索引 i = (x<<8)|(z<<4)|y）
    tLiqBits.assign(64, 0);
    if (hasLayer1) {
        uint16_t liqW = 0xFFFF, liqF = 0xFFFF;
        for (size_t i = 0; i < layer1.palette.size(); i++) {
            std::string const& n = layer1.palette[i];
            if (n == "water") liqW = (uint16_t)i;
            else if (n == "flowing_water") liqF = (uint16_t)i;
        }
        if (layer1.bitsPerIndex == 0 || layer1.words.empty()) {
            // 单一方块调色板: 整层同一方块
            if (!layer1.palette.empty()) {
                std::string const& n = layer1.palette[0];
                if (n == "water" || n == "flowing_water") tLiqBits.assign(64, ~0ull);
            }
        } else if (liqW != 0xFFFF || liqF != 0xFFFF) {
            uint32_t const mask = (1u << layer1.bitsPerIndex) - 1;
            int i = 0;
            for (int w = 0; w < layer1.wordCount && i < 4096; w++) {
                uint32_t word = layer1.words[w];
                for (int k = 0; k < layer1.blocksPerWord && i < 4096; k++, i++) {
                    uint32_t idx = (word >> (k * layer1.bitsPerIndex)) & mask;
                    if (idx == liqW || idx == liqF) tLiqBits[i >> 6] |= (1ull << (i & 63));
                }
            }
        }
    }

    // 单一方块调色板: 整个 subchunk 是同一种方块
    if (layer0.bitsPerIndex == 0 || layer0.words.empty()) {
        bool const allAir   = (airIdx == 0);
        bool const allWater = (waterIdx == 0) || (flowIdx == 0);
        bool const anyLiq   = (tLiqBits[0] != 0);
        uint8_t const v     = tVerdict.empty() ? 3 : tVerdict[0];
        for (int c = 0; c < 256; c++) {
            if (allAir || allWater) {
                out[c].topSolidY  = -1;
                out[c].waterAbove = (uint16_t)((allWater || anyLiq) ? 16 : 0);
            } else {
                out[c].topSolidY  = 15;      // 全实体: 最高位在 y=15
                out[c].waterAbove = 0;
                out[c].verdict    = v;
            }
        }
        return;
    }

    // 常规路径: 按 word 顺序解码（同一列的 y 递增出现, 用"更高的实体方块覆盖并清零其上水计数"增量维护,
    // 等价于 scanChunkSurface 里自顶向下逐格走的结果）
    uint32_t const mask = (1u << layer0.bitsPerIndex) - 1;
    int i = 0;
    for (int w = 0; w < layer0.wordCount && i < 4096; w++) {
        uint32_t word = layer0.words[w];
        for (int k = 0; k < layer0.blocksPerWord && i < 4096; k++, i++) {
            uint32_t idx = (word >> (k * layer0.bitsPerIndex)) & mask;
            int const x = (i >> 8) & 15;
            int const z = (i >> 4) & 15;
            int const y = i & 15;
            ColProbe& pr = out[x * 16 + z];

            bool const isAir   = (idx == airIdx);
            bool const isWater = (idx == waterIdx) || (idx == flowIdx);
            if (isAir) {
                if ((tLiqBits[i >> 6] >> (i & 63)) & 1ull) {
                    if (y > pr.topSolidY) pr.waterAbove++;
                }
                continue;
            }
            if (isWater) {
                if (y > pr.topSolidY) pr.waterAbove++;
                continue;
            }
            // 实体方块: 比已记录的更高则替换并清零其上水计数
            if (y > pr.topSolidY) {
                pr.topSolidY  = (int16_t)y;
                pr.waterAbove = 0;
                pr.verdict    = (idx < tVerdict.size()) ? tVerdict[idx] : 3;
            }
        }
    }
}

bool BedrockLevelReader::buildLandings(std::vector<std::string> const& dangerShort,
                                       std::function<void(size_t, size_t)> const& onProgress) {
    if (!mOpened) return false;
    if (cancelled()) return false;

    std::unordered_set<std::string> dangerSet;
    dangerSet.reserve(dangerShort.size() * 2 + 1);
    for (auto& d : dangerShort) dangerSet.insert(d);

    struct SubRef {
        int8_t   subIdx;
        uint32_t blockIdx;
        uint8_t const* key;
        uint8_t  keyLen;
    };
    std::vector<SubRef> subs;
    subs.reserve(32);

    for (int dim = 0; dim <= 2; dim++) {
        auto& table = mLandings[dim];
        table.clear();
        if (cancelled()) return false;

        bool const    useDim      = (dim != 0) || mOverworldKeyHasDim;
        uint8_t const expectedLen = useDim ? 14 : 10;
        uint8_t const tagOffset   = useDim ? 12 : 8;

        // 先数一遍该维度的 chunk 总数（进度日志用）
        size_t totalChunks = 0;
        {
            int32_t lastCX = INT32_MAX, lastCZ = INT32_MAX;
            for (auto const& e : mIndex) {
                if (e.keyLen != expectedLen || e.key[tagOffset] != 0x2F) continue;
                if (useDim && (int32_t)rdLE32_(e.key + 8) != dim) continue;
                int32_t const cx = (int32_t)rdLE32_(e.key);
                int32_t const cz = (int32_t)rdLE32_(e.key + 4);
                if (cx == lastCX && cz == lastCZ) continue;
                lastCX = cx; lastCZ = cz;
                totalChunks++;
            }
        }
        table.reserve(totalChunks);

        uint8_t  resolved[256];
        int16_t  finalY[256];
        uint16_t carried[256];

        size_t doneChunks = 0;
        size_t i = 0;
        while (i < mIndex.size()) {
            if ((i & 0x3FFF) == 0 && cancelled()) return false;
            KeyEntry const& e = mIndex[i];
            if (e.keyLen != expectedLen || e.key[tagOffset] != 0x2F) { i++; continue; }
            if (useDim && (int32_t)rdLE32_(e.key + 8) != dim) { i++; continue; }
            int32_t const cx = (int32_t)rdLE32_(e.key);
            int32_t const cz = (int32_t)rdLE32_(e.key + 4);

            // 收集本 chunk 的 subchunk（同前缀条目在索引中连续）
            subs.clear();
            size_t j = i;
            while (j < mIndex.size()) {
                KeyEntry const& f = mIndex[j];
                if (f.keyLen != expectedLen || f.key[tagOffset] != 0x2F) break;
                if (useDim && (int32_t)rdLE32_(f.key + 8) != dim) break;
                if ((int32_t)rdLE32_(f.key) != cx || (int32_t)rdLE32_(f.key + 4) != cz) break;
                if (f.keyLen >= (uint8_t)(tagOffset + 2)) {
                    subs.push_back(SubRef{(int8_t)f.key[tagOffset + 1], f.blockIdx, f.key, f.keyLen});
                }
                j++;
            }
            i = j;
            if (subs.empty()) continue;

            // 自上而下（subIdx 降序）
            std::sort(subs.begin(), subs.end(),
                      [](SubRef const& a, SubRef const& b) { return a.subIdx > b.subIdx; });

            for (int c = 0; c < 256; c++) { resolved[c] = 0; finalY[c] = 0; carried[c] = 0; }
            int remaining = 256;

            for (SubRef const& sr : subs) {
                auto block = loadBlock(sr.blockIdx);
                if (!block) continue;
                uint8_t const* vPtr = nullptr;
                size_t vLen = 0;
                if (!findInBlockPtr(block->data(), block->size(), sr.key, sr.keyLen, vPtr, vLen)) continue;

                ColProbe probes[256];
                decodeSubchunkColumns(vPtr, vLen, dangerSet, probes);

                int const base = (int)sr.subIdx * 16;
                for (int c = 0; c < 256 && remaining > 0; c++) {
                    if (resolved[c]) continue;
                    ColProbe const& pr = probes[c];
                    if (pr.topSolidY >= 0) {
                        uint32_t const wd = (uint32_t)carried[c] + pr.waterAbove;
                        finalY[c]   = (int16_t)(base + pr.topSolidY);
                        resolved[c] = (pr.verdict == 1 && wd <= 1) ? 1 : 2;
                        remaining--;
                    } else {
                        uint32_t const sum = (uint32_t)carried[c] + pr.waterAbove;
                        carried[c] = (uint16_t)std::min<uint32_t>(sum, 65535u);
                    }
                }
                // 地形在最高 1~3 个 subchunk 内就定完了 → 提前收工
                if (remaining == 0) break;
            }

            // 取离 chunk 中心最近的可用列（平局规则与 findSafeInSurface 一致）
            int bestCol = -1, bestDist = INT_MAX;
            for (int lx = 0; lx < 16; lx++) {
                for (int lz = 0; lz < 16; lz++) {
                    int const c = lx * 16 + lz;
                    if (resolved[c] != 1) continue;
                    int const dist = std::abs(lx - 8) + std::abs(lz - 8);
                    if (dist < bestDist) { bestDist = dist; bestCol = c; }
                }
            }

            Landing ld;
            ld.cx = cx;
            ld.cz = cz;
            if (bestCol >= 0) {
                ld.state = LAND_SAFE;
                ld.y     = (int32_t)finalY[bestCol] + 1;   // 站在地表之上
                ld.lx    = (int8_t)(bestCol / 16);
                ld.lz    = (int8_t)(bestCol % 16);
            } else {
                ld.state = LAND_NO_SAFE;
            }
            table.push_back(ld);

            doneChunks++;
            if (onProgress && (doneChunks & 0xFFF) == 0) onProgress(doneChunks, totalChunks);
        }

        // 索引按 key 字节序排列（cx/cz 小端逐字节比较, 与数值序不同）→ 重排后才能二分
        std::sort(table.begin(), table.end(), [](Landing const& a, Landing const& b) {
            if (a.cx != b.cx) return a.cx < b.cx;
            return a.cz < b.cz;
        });
        if (onProgress) onProgress(doneChunks, totalChunks);
    }

    mLandingsReady.store(true, std::memory_order_release);
    return true;
}

bool BedrockLevelReader::lookupLanding(int cx, int cz, int dim, Landing& out) const {
    if (dim < 0 || dim > 2) return false;
    if (!mLandingsReady.load(std::memory_order_acquire)) return false;
    auto const& table = mLandings[dim];
    if (table.empty()) return false;

    size_t lo = 0, hi = table.size();
    while (lo < hi) {
        size_t const mid = (lo + hi) / 2;
        Landing const& m = table[mid];
        if (m.cx < cx || (m.cx == cx && m.cz < cz)) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= table.size()) return false;
    Landing const& m = table[lo];
    if (m.cx != cx || m.cz != cz) return false;
    out = m;
    return true;
}

size_t BedrockLevelReader::landingCount() const {
    size_t n = 0;
    for (int d = 0; d < 3; d++) n += mLandings[d].size();
    return n;
}
