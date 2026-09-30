#pragma once
// 地表判定共用规则：内存扫描（findSurfaceSafe）/ 同步存档读取（scanChunkSurface）/
// 落点预计算（decodeSubchunkColumns）三处必须一致，否则同一坐标在不同数据源上会给出不同落点。
#include <string>

namespace mtps {

// 树叶不算地表。
// 表面扫描是"从 y=200 往下找第一个非空气方块"，而基岩版树叶是实心方块，于是整片森林的落点
// 都停在树冠顶上——玩家得砸树叶才下得来。实测出生点附近树叶占陆地列 14%，落点表逐 chunk 对照
// 有 24% 的落点因此下移 9~30 格。
// 判定按后缀，不列名字表：树种会随版本增加，后缀一次全覆盖。
// 注意别塞进 dangerBlocks：那边的语义是"整列不要"，这里要的是"跳过它继续往下找"。
inline bool isLeafBlockName(std::string const& n) {
    constexpr char   kSuffix[] = "_leaves";
    constexpr size_t kLen      = sizeof(kSuffix) - 1;
    return n.size() > kLen && n.compare(n.size() - kLen, kLen, kSuffix) == 0;
}

// 落点规则版本：规则一改就换，否则老的 landings.cache 会被继续复用（缓存指纹只看 .ldb，不看规则）。
inline constexpr char kLandingRulesVersion[] = "v2-leaf-transparent";

} // namespace mtps
