#pragma once
// 与 MeowPAPI 的 ABI 对接。
// 直接解析 MeowPAPI.dll 导出的 C 接口，不链接它的静态库，也就不依赖 lrca。
// MeowPAPI 没装或版本不符时全部静默跳过，占位符不注册而已。
#include <cstdint>
#include <string>

namespace mtps {
namespace papi {

// 解析 ABI 是否可用（DLL 已加载且关键导出齐全）
bool available();

// 注册本插件的全部占位符（按 Config 的 papi 段决定注册哪些）
// 重复调用是幂等的（先按插件名反注册再注册）
void registerAll();

// 反注册本插件的占位符（禁用插件时调用，避免回调指向已卸载的代码）
void unregisterAll();

// 供日志使用的一句话状态
std::string statusText();

} // namespace papi
} // namespace mtps
