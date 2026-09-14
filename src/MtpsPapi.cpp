#include "MtpsPapi.h"

#include "Config.h"
#include "DataStore.h"
#include "TpaRally.h"

#include <ll/api/service/Bedrock.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>

#include <unordered_map>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace mtps {
namespace papi {

namespace {

// MeowPAPI ABI（只用到的部分；类型按它的 DllExports.h）
using CallbackFnV2 = int (*)(uint64_t callbackId, void* player, char* out, int outSize);
using GetAbiVersionFn = uint32_t (*)();
using AllocCallbackIdFn = uint64_t (*)();
using SetInvokerV2Fn = void (*)(CallbackFnV2 fn);
using RegisterPlaceholderFn = int (*)(char const* plugin, char const* name, int type, uint64_t cbId, int intervalMs);
using UnregisterByPluginFn = void (*)(char const* plugin);

constexpr int kTypePlayer = 1;
constexpr uint32_t kAbiMajor = 1;
constexpr char const* kPluginName = "Mtps";

struct Api {
    bool     ok{false};
    GetAbiVersionFn       getAbiVersion{nullptr};
    AllocCallbackIdFn     allocCallbackId{nullptr};
    SetInvokerV2Fn        setInvokerV2{nullptr};
    RegisterPlaceholderFn registerPlaceholder{nullptr};
    UnregisterByPluginFn  unregisterByPlugin{nullptr};
};

Api& api() {
    static Api a;
    static bool tried = false;
    if (a.ok || tried) return a;
    tried = true;
#ifdef _WIN32
    HMODULE h = GetModuleHandleW(L"MeowPAPI.dll");
    if (!h) return a;   // 未安装：不主动加载，保持软依赖
    a.getAbiVersion       = reinterpret_cast<GetAbiVersionFn>(GetProcAddress(h, "MeowPAPI_GetAbiVersion"));
    a.allocCallbackId     = reinterpret_cast<AllocCallbackIdFn>(GetProcAddress(h, "MeowPAPI_AllocCallbackId"));
    a.setInvokerV2        = reinterpret_cast<SetInvokerV2Fn>(GetProcAddress(h, "MeowPAPI_SetCallbackInvokerV2"));
    a.registerPlaceholder = reinterpret_cast<RegisterPlaceholderFn>(GetProcAddress(h, "MeowPAPI_RegisterPlaceholder"));
    a.unregisterByPlugin  = reinterpret_cast<UnregisterByPluginFn>(GetProcAddress(h, "MeowPAPI_UnregisterByPlugin"));
    if (!a.getAbiVersion || !a.allocCallbackId || !a.setInvokerV2 || !a.registerPlaceholder) return a;
    if ((a.getAbiVersion() >> 16) != kAbiMajor) return a;
    a.ok = true;
#endif
    return a;
}

// 每个占位符一个回调 id（回调只带 id，不带名字，必须一一对应）
std::unordered_map<uint64_t, std::string>& idToName() {
    static std::unordered_map<uint64_t, std::string> m;
    return m;
}

// 把文本写进 MeowPAPI 给的缓冲区（它保证 outSize 含结尾 0）
void writeOut(char* out, int outSize, std::string const& text) {
    if (out == nullptr || outSize <= 0) return;
    int const n = (int)text.size() < outSize - 1 ? (int)text.size() : outSize - 1;
    for (int i = 0; i < n; i++) out[i] = text[i];
    out[n] = 0;
}

// 取值：按名字算出文本（Config 里的 format 决定长什么样）
std::string render(std::string const& name, Player* player) {
    auto& cfg = Config::getInstance();
    std::string fmt = cfg.papiFormat(name);
    if (fmt.empty()) return "";

    std::string playerName = player ? player->getRealName() : std::string{};
    std::string xuid = player ? player->getXuid() : std::string{};

    auto replaceAll = [](std::string& s, std::string const& from, std::string const& to) {
        if (from.empty()) return;
        for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) {
            s.replace(pos, from.size(), to);
        }
    };

    if (name == "mtps_tpa") {
        // 无人请求时返回空串（侧边栏那条就不显示）
        std::string fromName, typeText;
        if (!TpaRally::getInstance().pendingIncoming(playerName, fromName, typeText)) return "";
        replaceAll(fmt, "{player}", fromName);
        replaceAll(fmt, "{type}", typeText);
        return fmt;
    }

    if (name == "mtps_tpa_count") {
        std::string fromName, typeText;
        bool has = TpaRally::getInstance().pendingIncoming(playerName, fromName, typeText);
        replaceAll(fmt, "{count}", has ? "1" : "0");
        return fmt;
    }

    if (name == "mtps_warps") {
        int count = xuid.empty() ? 0 : DataStore::getInstance().getPrivateWarpCount(xuid);
        replaceAll(fmt, "{count}", std::to_string(count));
        replaceAll(fmt, "{max}", std::to_string(cfg.maxPrivateWarps()));
        return fmt;
    }

    if (name == "mtps_shared") {
        int count = 0;
        if (!xuid.empty()) {
            for (auto& w : DataStore::getInstance().getPrivateWarps(xuid)) {
                if (w.noApproval) count++;
            }
        }
        replaceAll(fmt, "{count}", std::to_string(count));
        return fmt;
    }

    if (name == "mtps_public") {
        replaceAll(fmt, "{count}", std::to_string(DataStore::getInstance().getPublicWarps().size()));
        return fmt;
    }

    return fmt;
}

// MeowPAPI 求值占位符时回调进来：返回 1 = 这个 id 归我们处理
int invoker(uint64_t callbackId, void* player, char* out, int outSize) {
    auto& map = idToName();
    auto  it  = map.find(callbackId);
    if (it == map.end()) return 0;   // 不是我们的 id，让链上其他插件处理
    writeOut(out, outSize, render(it->second, static_cast<Player*>(player)));
    return 1;
}

// 目前注册的占位符（用于反注册与状态）
std::vector<std::string>& registered() {
    static std::vector<std::string> v;
    return v;
}

} // namespace

bool available() { return api().ok; }

std::string statusText() {
    if (!api().ok) return "MeowPAPI ABI 不可用（未安装或版本不符），占位符未注册";
    return "MeowPAPI ABI 已接管，已注册 " + std::to_string(registered().size()) + " 个占位符";
}

void registerAll() {
    auto& a = api();
    if (!a.ok) return;

    // 重复调用先清掉上一轮的登记
    if (a.unregisterByPlugin) a.unregisterByPlugin(kPluginName);
    idToName().clear();
    registered().clear();

    a.setInvokerV2(&invoker);   // 链式：只处理自己的 id

    auto& cfg = Config::getInstance();
    for (auto const& name : Config::papiNames()) {
        if (cfg.papiFormat(name).empty()) continue;   // 未启用/未配置
        uint64_t const id = a.allocCallbackId();
        if (id == 0) continue;
        if (a.registerPlaceholder(kPluginName, name.c_str(), kTypePlayer, id, 0)) {
            idToName()[id] = name;
            registered().push_back(name);
        }
    }
}

void unregisterAll() {
    auto& a = api();
    if (!a.ok) return;
    if (a.unregisterByPlugin) a.unregisterByPlugin(kPluginName);
    idToName().clear();
    registered().clear();
}

} // namespace papi
} // namespace mtps
