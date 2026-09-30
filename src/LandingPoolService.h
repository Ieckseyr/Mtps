#pragma once
// 预落点池的开服装配与后台补齐，全在后台线程做：
//   等世界与种子就绪 → 检测 BDS 版本 → 配 BiomeSampler → 载入落盘池 → 推导各维度生成区域
//   → 逐维度补齐缺口 → 落盘
#include <string>

namespace mtps {

// 启动后台补齐线程（幂等），由 Mtps 启用流程在 ArchiveScanner 之后调
void startLandingPoolService();
// 停线程并等待结束（关服 / 重载）
void stopLandingPoolService();

// 从配置里的随机传送预设推导各维度区域（取该维度上半径最大的启用预设）同步给池，
// 配置保存 / reload / 启用时调。区域变更只打"待重建"标记，重算在后台做。
void syncPoolRegionsFromPresets();

} // namespace mtps
