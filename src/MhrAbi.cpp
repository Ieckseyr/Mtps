#include "MhrAbi.h"

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace mtps {
namespace mhrabi {

namespace {

// ABI 主版本: MHR 侧递增主版本号即表示不兼容变更, 此时本插件退回自建
constexpr uint32_t kSupportedMajor = 1;   // 与 MHR_GetAbiVersion 的主版本对齐

using GetAbiFn  = uint32_t (*)();
using SpawnFn   = int64_t (*)(char const*, char const*, char const*, float, float, float, int, float, float, int64_t*);
using DespawnFn = bool (*)(char const*);

struct Fns {
    SpawnFn   spawn{nullptr};
    DespawnFn despawn{nullptr};
};

// 解析一次成功后缓存; 失败不缓存（插件加载顺序不定, 对方稍后才就绪也能接上）
Fns& resolve() {
    static Fns f;
    if (f.spawn != nullptr) return f;

#ifdef _WIN32
    auto& log = ll::mod::NativeMod::current()->getLogger();
    HMODULE h = GetModuleHandleW(L"MeowHolographicRenderer.dll");
    if (!h) {
        log.info("[MHR] 未加载, 虚假实体由本插件自建");
        return f;
    }
    auto getAbi = reinterpret_cast<GetAbiFn>(GetProcAddress(h, "MHR_GetAbiVersion"));
    auto sp     = reinterpret_cast<SpawnFn>(GetProcAddress(h, "MHR_SpawnEntity"));
    auto dp     = reinterpret_cast<DespawnFn>(GetProcAddress(h, "MHR_DespawnEntity"));
    if (!getAbi || !sp || !dp) {
        log.warn("[MHR] 已加载但缺少 ABI 导出（需要带 MHR_SpawnEntity 的版本）, 自建实体");
        return f;
    }
    uint32_t const abi = getAbi();
    if ((abi >> 16) != kSupportedMajor) {
        log.warn("[MHR] ABI 主版本不符（对方 0x{:06X}, 本插件支持 {}）, 自建实体", abi, kSupportedMajor);
        return f;
    }
    f.spawn   = sp;
    f.despawn = dp;
#endif
    return f;
}

} // namespace

bool available() { return resolve().spawn != nullptr; }

int64_t spawn(std::string const& ownerKey, std::string const& identifier, std::string const& nametag,
              float x, float y, float z, int dim, float yaw, float scale, int64_t* outLibId) {
    auto& f = resolve();
    if (!f.spawn || ownerKey.empty() || identifier.empty()) return -1;
    return f.spawn(ownerKey.c_str(), identifier.c_str(), nametag.c_str(), x, y, z, dim, yaw, scale, outLibId);
}

bool despawn(std::string const& ownerKey) {
    auto& f = resolve();
    if (!f.despawn || ownerKey.empty()) return false;
    return f.despawn(ownerKey.c_str());
}

} // namespace mhrabi
} // namespace mtps
