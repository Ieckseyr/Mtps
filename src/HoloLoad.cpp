#include "HoloLoad.h"

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

#include <windows.h>

#include <hologramlib/HologramLib.h>

#include <filesystem>

namespace mtps {

namespace {

constexpr char const* kHoloDll = "HologramLib.dll";

// Mtps.dll 自己的目录（用来推出 plugins/HologramLib/HologramLib.dll, 不依赖工作目录）
std::filesystem::path selfDir() {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&selfDir), &self)) {
        return {};
    }
    wchar_t buf[MAX_PATH]{};
    if (::GetModuleFileNameW(self, buf, MAX_PATH) == 0) return {};
    return std::filesystem::path(buf).parent_path();
}

} // namespace

namespace {
hologramlib::IHologramLib* sHolo = nullptr;
}

hologramlib::IHologramLib* holoInstance() { return sHolo; }

bool preloadHologramLib() {
    if (sHolo != nullptr) return true;
    // ① 已经在进程里的（LeviLamina 按 mod 依赖先加载它的情况）
    if (::GetModuleHandleA(kHoloDll) != nullptr) {
        sHolo = &hologramlib::IHologramLib::getInstance();   // 唯一一次过延迟加载桩
        return true;
    }

    // ② 插件目录: <Mtps.dll 所在目录>/../HologramLib/HologramLib.dll
    std::filesystem::path const dir = selfDir();
    if (!dir.empty()) {
        auto const p = dir / ".." / "HologramLib" / kHoloDll;
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES
            && ::LoadLibraryW(p.c_str()) != nullptr) {
            sHolo = &hologramlib::IHologramLib::getInstance();
            return true;
        }
    }
    // ③ 工作目录下的标准位置（服务器根/plugins/HologramLib/）
    if (::LoadLibraryA("plugins/HologramLib/HologramLib.dll") != nullptr) {
        sHolo = &hologramlib::IHologramLib::getInstance();
        return true;
    }

    ll::mod::NativeMod::current()->getLogger().error(
        "[HologramLib] 没有找到 HologramLib.dll（找过: 已加载模块 / <插件目录>/../HologramLib/ / "
        "plugins/HologramLib/）; 请确认它已放进 plugins/HologramLib/ 并且自己加载成功; "
        "本次 Mtps 停用（不会在 tick 里再抛延迟加载异常）");
    return false;
}

} // namespace mtps
