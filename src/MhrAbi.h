#pragma once
// MhrAbi.h - 与 MeowHolographicRenderer 的 C ABI 软对接（GetProcAddress + 版本握手）。
// 不用 lrca/RemoteCall: 那是按名字的运行期调用, 不透明也不好版本化; 对方没装/主版本不符
// 就整条链路静默失效, 退回自建实体, 不构成硬依赖。
// 约定: MHR_GetAbiVersion / MHR_SpawnEntity(ownerKey 幂等) / MHR_DespawnEntity。
#include <cstdint>
#include <string>

namespace mtps {
namespace mhrabi {

// MHR 已加载且 ABI 主版本兼容
bool available();

// 创建/复用（按 ownerKey 幂等）一个由 MHR 管理的协议实体; 返回其 uid, <= 0 表示失败
//   x/y/z 传"期望的可视位置"即可（库的 y 偏移补偿由 MHR 内部处理）
int64_t spawn(
    std::string const& ownerKey,
    std::string const& identifier,
    std::string const& nametag,
    float              x,
    float              y,
    float              z,
    int                dim,
    float              yaw,
    float              scale,
    int64_t*           outLibId   // 出参: 回传 HologramLib 运行时实体 id（点击路由用）; 可 nullptr
);

// 按 ownerKey 移除（连带库内实体与持久化记录）
bool despawn(std::string const& ownerKey);

} // namespace mhrabi
} // namespace mtps
