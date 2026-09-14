#pragma once
// MenuCommon.h - 菜单模块共用的小工具
// （原先散在 Menu.cpp 里; 管理表单拆到 AdminMenu.cpp 后需要两边共用）
#include <string>
#include <variant>
#include <vector>

#include "Config.h"

#include <ll/api/form/CustomForm.h>
#include <mc/world/actor/player/Player.h>

namespace mtps {
namespace menu {

// 按钮图标
namespace tex {
inline constexpr char const* WARP      = "textures/ui/world_glyph_color";
inline constexpr char const* PLAYER    = "textures/ui/icon_multiplayer";
inline constexpr char const* REQUEST   = "textures/ui/message";
inline constexpr char const* SETTINGS  = "textures/ui/settings_glyph_color_2x";
inline constexpr char const* BLACKLIST = "textures/ui/cancel";
inline constexpr char const* PRIVATE   = "textures/ui/icon_lock";
inline constexpr char const* PUBLIC    = "textures/ui/icon_multiplayer";
inline constexpr char const* ADD       = "textures/ui/color_plus";
inline constexpr char const* DELETE    = "textures/ui/trash_default";
inline constexpr char const* TELEPORT  = "textures/ui/arrow_right";
inline constexpr char const* BACK      = "textures/ui/arrow_left";
inline constexpr char const* ACCEPT    = "textures/ui/check";
inline constexpr char const* REFUSE    = "textures/ui/cancel";
inline constexpr char const* CANCEL    = "textures/ui/realms_red_x";
inline constexpr char const* INFO      = "textures/ui/magnifyingGlass";
} // namespace tex

inline bool isOp(::Player& p) { return (int)p.getCommandPermissionLevel() >= 1; }

inline void tell(::Player& p, std::string const& msg) { p.sendMessage(msg); }

// 费用文本（菜单里多处用, 所以放共用头）
inline std::string formatCost(int cost) {
    if (cost <= 0) return "§a免费§r";
    return "§e" + std::to_string(cost) + " " + Config::getInstance().moneyName() + "§r";
}

// 换行转义: 表单输入框是单行的, 所以约定用 "\n" 两个字符表示换行（与 MHR 一致）。
// 这一对必须成对使用, 否则会不对称: 存的时候解码、回填的时候编码,
// 不然真换行塞进单行控件会显示成乱码, 而且管理员再点一次确认就把乱码存回去了。
inline std::string decodeNl(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

inline std::string encodeNl(std::string const& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') continue;      // 规范化: 只留 \n
        else out += c;
    }
    return out;
}

// CustomForm 回调取值: 结果类型是 variant<monostate, uint64, double, string>, 同一控件在不同
// 客户端可能回下标也可能回文本, 所以这里统一兼容, 免得"选了第二项却按第一项处理"。
inline double formGetNumber(ll::form::CustomFormResult const& res, std::string const& key, double def = 0) {
    auto it = res->find(key);
    if (it == res->end()) return def;
    if (auto* v = std::get_if<double>(&it->second)) return *v;
    if (auto* v = std::get_if<uint64>(&it->second)) return (double)*v;
    if (auto* v = std::get_if<std::string>(&it->second)) {
        try {
            return std::stod(*v);
        } catch (...) {
            return def;
        }
    }
    return def;
}

inline bool formGetBool(ll::form::CustomFormResult const& res, std::string const& key, bool def = false) {
    auto it = res->find(key);
    if (it == res->end()) return def;
    if (auto* v = std::get_if<uint64>(&it->second)) return *v != 0;
    if (auto* v = std::get_if<double>(&it->second)) return *v != 0.0;
    if (auto* v = std::get_if<std::string>(&it->second)) return (*v == "true" || *v == "1");
    return def;
}

inline std::string formGetString(ll::form::CustomFormResult const& res, std::string const& key,
                                 std::string const& def = {}) {
    auto it = res->find(key);
    if (it == res->end()) return def;
    if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    if (auto* v = std::get_if<uint64>(&it->second)) return std::to_string(*v);
    if (auto* v = std::get_if<double>(&it->second)) {
        // 滑块值可能是 3 或 3.0, 去掉多余小数位
        double d = *v;
        if (d == (double)(long long)d) return std::to_string((long long)d);
        return std::to_string(d);
    }
    return def;
}

// 步进滑块: 结果可能是下标, 也可能是被选中的文本, 两种都还原成下标
inline int formGetStep(ll::form::CustomFormResult const& res, std::string const& key,
                       std::vector<std::string> const& options, int def = 0) {
    auto it = res->find(key);
    if (it == res->end()) return def;
    if (auto* v = std::get_if<uint64>(&it->second)) return (int)*v;
    if (auto* v = std::get_if<double>(&it->second)) return (int)*v;
    if (auto* v = std::get_if<std::string>(&it->second)) {
        std::string const& t = *v;
        bool numeric = !t.empty();
        for (char c : t) {
            if (c < '0' || c > '9') { numeric = false; break; }
        }
        if (numeric) return std::stoi(t);
        for (size_t k = 0; k < options.size(); k++) {   // 文本匹配回下标
            if (t == options[k]) return (int)k;
        }
    }
    return def;
}

} // namespace menu
} // namespace mtps
