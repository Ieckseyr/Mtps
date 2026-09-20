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

namespace hologramlib {
class IHologramLib;
}

namespace mtps {

// 显式把 HologramLib.dll 载进来并在成功后缓存 IHologramLib 实例
// （enable 开头调用; 失败会打日志并返回 false）
bool preloadHologramLib();

// 缓存的 HologramLib 实例; 没解析成功返回 nullptr。
// 之所以要缓存: IHologramLib::getInstance() 是 Mtps.dll 里唯一一条"延迟加载"导入,
// 每调用一次都要过一遍延迟加载桩 —— 那个桩在解析失败时是抛 SEH 异常（0xC06D007E）而不是
// 返回空, 放在 tick 热路径上就等于给服务器埋雷（已经炸过: 服务器 tick 里抛, 整进程退出）。
hologramlib::IHologramLib* holoInstance();

// 便捷取用（仅在确认已就绪的路径上用, 比如 enable 成功之后的 tick/交互）
inline hologramlib::IHologramLib& holo() { return *holoInstance(); }
inline bool holoReady() { return holoInstance() != nullptr; }

} // namespace mtps
