#pragma once
// HologramLib 是"按名字"被 Mtps.dll 导入的。原来直接静态导入（add_links("HologramLib")）,
// 后果是: 只要 HologramLib.dll 还没加载（或它自己加载失败), Windows 就直接拒绝加载 Mtps.dll,
// LeviLamina 只会打一行"无法加载 Mtps", 看不出原因, 也跟我们的代码无关。
// 现在改成延迟加载（xmake 里的 /DELAYLOAD）: Mtps.dll 先正常加载, 用到时才去解析 ——
// 首次解析发生在 enable(), 由 preloadHologramLib() 按下面的顺序把 DLL 捞进来:
//   ① 进程里已经加载的同名模块（LeviLamina 按 mod 依赖先加载它的情况）
//   ② plugins/HologramLib/HologramLib.dll（以 Mtps.dll 自身位置为基准, 不受工作目录影响）
//   ③ 工作目录下的 plugins/HologramLib/HologramLib.dll
// 注意: 延迟加载的 stub 是按名字找已加载模块的, 所以这里必须先 LoadLibrary 成功 ——
// 捞不到就明确报错并让 enable() 失败, 而不是整个插件在"无法加载 Mtps"里静默消失。

namespace mtps {

// 显式把 HologramLib.dll 载进来（enable 开头调用; 失败会打日志并返回 false）
bool preloadHologramLib();

} // namespace mtps
