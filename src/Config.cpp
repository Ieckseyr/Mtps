#include "Config.h"
#include <fstream>
#include <stdexcept>
#include <algorithm>

namespace mtps {

static fs::path const sDataDir = "Meowdata/Mtps";

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

fs::path const& Config::dataDir() {
    static fs::path d = "Meowdata/Mtps";
    return d;
}
fs::path Config::configPath()          { return dataDir() / "Config.json"; }
fs::path Config::privateWarpsPath()    { return dataDir() / "PrivateWarps.json"; }
fs::path Config::publicWarpsPath()     { return dataDir() / "PublicWarps.json"; }
fs::path Config::tpaCachePath()        { return dataDir() / "TpaCachePool.json"; }
fs::path Config::personalRulesPath()   { return dataDir() / "PersonalRuleSettings.json"; }
fs::path Config::warpRequestsPath()    { return dataDir() / "WarpRequests.json"; }
fs::path Config::blockTpPointsPath()   { return dataDir() / "BlockTeleportPoints.json"; }
fs::path Config::crossServerPath()     { return dataDir() / "CrossServer.json"; }
fs::path Config::npcSkinsDir()         { return dataDir() / "npc_skins"; }

json Config::defaultConfig() const {
    return R"({
        "economy": { "enabled": false, "type": "llmoney", "scoreboardName": "money", "moneyName": "金币",
            "costs": { "publicWarp": 0, "privateWarp": 50, "noApprovalWarp": 50, "tpa": 100, "rally": 0, "randomTeleport": 0 } },
        "teleport": { "cooldownTime": 30, "effectiveDuration": 60, "maxPrivateWarps": 5 },
        "rally": { "enabled": true, "duration": 60, "cooldown": 60, "notifySound": false },
        "defaultRules": { "refuseAllTpaRequest": false, "noPopUpWindow": false, "refuseAllWarpRequests": false },
        "logging": { "logToConsole": true, "broadcastToGame": true },
        "notification": {
            "sound": { "enabled": true, "soundName": "mob.endermen.portal", "pitch": 1, "volume": 1 },
            "sidebar": { "enabled": true, "format": "§6[TPA] §e{player} §7{type}" },
            "texts": {
                "tpaToMe": "想传送到你身边", "tpaMeTo": "想让你传送到TA身边",
                "warpApplyType": "申请传送到您的传送点",
                "popupTitle": "§l§6传送请求",
                "popupContent": "§e{player}§r {type}\n\n§b请求时间: §e{time}\n§b有效时长: §e{duration}秒",
                "chatNotify": "§e{player}§r {type}，使用 /mtps 处理,或使用§a /y 同意 §c /n 拒绝"
            }
        },
        "features": {
            "warpSystem": true, "privateWarps": true, "publicWarps": true,
            "noApprovalWarps": true, "playerWarpBrowser": true,
            "tpa": true, "rally": true, "randomTeleport": true, "crossServer": true
        },
        "blockTeleport": {
            "enabled": true,
            "quickAdd": { "item": "minecraft:nether_star", "requireSneak": true, "requireAdmin": true, "cooldownMs": 1000 },
            "fixedCooldownMs": 200,
            "editTool": {
                "enabled": true,
                "item": "minecraft:nether_star",
                "itemRequiresSneak": false,
                "sneak": true,
                "requireAdmin": true
            }
        },
        "randomTeleport": {
            "enabled": true, "cooldownSeconds": 120, "maxAttempts": 50,
            "debug": true,
            "dangerBlocks": [ "minecraft:lava","minecraft:flowing_lava","minecraft:water","minecraft:flowing_water",
                "minecraft:fire","minecraft:soul_fire","minecraft:cactus","minecraft:sweet_berry_bush",
                "minecraft:magma_block","minecraft:wither_rose","minecraft:powder_snow" ],
            "presets": [
                { "name": "主世界随机", "enabled": true, "originMode": "fixed", "originX": 0, "originZ": 0,
                  "radius": 1000, "dimid": 0, "message": "§a[随机传送] §f已传送到主世界随机位置！",
                  "economy": { "cost": 0, "type": "default" } }
            ]
        },
        "commands": {
            "enabled": true, "menu": "mtps", "warp": "warp", "private": "pw", "tpa": "tpa",
            "rally": "call", "requests": "req", "settings": "set",
            "blacklist": "blk", "admin": "adm", "blocktp": "btp", "random": "tpr",
            "public": "pub", "noapproval": "nap", "browser": "list", "crossserver": "cs",
            "reload": "rld", "accept": "y", "refuse": "n"
        },
        "skins": {
            "extraDirs": [
                "plugins/MeowHolographicRenderer/config/npc_skins",
                "plugins/MeowHolographicRenderer/skins"
            ]
        }
    })"_json;
}

// 类型安全读取：配置项缺失或类型不符一律回落默认值（不抛异常、不整份重置）
namespace {
bool jbool(json const& o, char const* k, bool def) {
    auto it = o.find(k);
    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : def;
}
int jint(json const& o, char const* k, int def) {
    auto it = o.find(k);
    return (it != o.end() && it->is_number_integer()) ? it->get<int>() : def;
}
float jfloat(json const& o, char const* k, float def) {
    auto it = o.find(k);
    return (it != o.end() && it->is_number()) ? it->get<float>() : def;
}
std::string jstr(json const& o, char const* k, char const* def) {
    auto it = o.find(k);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string{def};
}
} // namespace

void Config::buildCaches() {
    // 危险方块（全名 + 短名）
    mDangerBlocksCache.clear();
    mDangerShortCache.clear();
    if (mConfig.contains("randomTeleport") && mConfig["randomTeleport"].contains("dangerBlocks")) {
        for (auto& b : mConfig["randomTeleport"]["dangerBlocks"]) {
            if (!b.is_string()) continue;
            std::string full = b.get<std::string>();
            mDangerShortCache.push_back(full.rfind("minecraft:", 0) == 0 ? full.substr(10) : full);
            mDangerBlocksCache.push_back(std::move(full));
        }
    }

    // 随机传送预设
    mPresetsCache.clear();
    if (mConfig.contains("randomTeleport") && mConfig["randomTeleport"].contains("presets")) {
        for (auto& p : mConfig["randomTeleport"]["presets"]) {
            RandomPreset rp;
            rp.name       = jstr(p, "name", "");
            rp.enabled    = jbool(p, "enabled", true);
            rp.originMode = jstr(p, "originMode", "fixed");
            rp.originX    = p.value("originX", 0.0);
            rp.originZ    = p.value("originZ", 0.0);
            rp.radius     = jint(p, "radius", 1000);
            rp.dimid      = jint(p, "dimid", 0);
            rp.message    = jstr(p, "message", "");
            rp.cooldown   = jint(p, "cooldown", 0);
            if (p.contains("economy") && p["economy"].is_object()) {
                rp.economy.enabled = jbool(p["economy"], "enabled", false);
                rp.economy.cost    = jint(p["economy"], "cost", 0);
                rp.economy.type    = jstr(p["economy"], "type", "default");
            }
            mPresetsCache.push_back(std::move(rp));
        }
    }

    // 功能开关 / 费用 / 通知文本 / 命令
    mFeatureCache.clear();
    if (mConfig.contains("features") && mConfig["features"].is_object()) {
        for (auto it = mConfig["features"].begin(); it != mConfig["features"].end(); ++it) {
            if (it.value().is_boolean()) mFeatureCache[it.key()] = it.value().get<bool>();
        }
    }
    mCostCache.clear();
    if (mConfig.contains("economy") && mConfig["economy"].is_object()) {
        auto const& costs = mConfig["economy"];
        if (costs.contains("costs") && costs["costs"].is_object()) {
            for (auto it = costs["costs"].begin(); it != costs["costs"].end(); ++it) {
                if (it.value().is_number_integer()) mCostCache[it.key()] = it.value().get<int>();
            }
        }
    }
    mNotifyTextCache.clear();
    if (mConfig.contains("notification") && mConfig["notification"].is_object()) {
        auto const& n = mConfig["notification"];
        if (n.contains("texts") && n["texts"].is_object()) {
            for (auto it = n["texts"].begin(); it != n["texts"].end(); ++it) {
                if (it.value().is_string()) mNotifyTextCache[it.key()] = it.value().get<std::string>();
            }
        }
    }
    // 指令名缓存: 先用出厂默认铺一遍, 再用配置覆盖
    // （这样配置里缺某个键时也是"回落到默认指令名", 而不是拿键名当指令名 —— 键名可能
    //   不是合法/期望的指令名, 比如 reload 这种比默认名长的键）
    mCommandCache.clear();
    {
        auto const defs = defaultConfig()["commands"];
        for (auto it = defs.begin(); it != defs.end(); ++it) {
            if (it.value().is_string()) mCommandCache[it.key()] = it.value().get<std::string>();
        }
    }
    if (mConfig.contains("commands") && mConfig["commands"].is_object()) {
        for (auto it = mConfig["commands"].begin(); it != mConfig["commands"].end(); ++it) {
            if (it.value().is_string()) mCommandCache[it.key()] = it.value().get<std::string>();
        }
    }

    // NPC 皮肤的额外来源目录（默认指向 MHR 的皮肤目录, 让 MHR 存过的皮肤这里直接可用）
    mSkinExtraDirs.clear();
    if (mConfig.contains("skins") && mConfig["skins"].is_object()) {
        auto const& sk = mConfig["skins"];
        if (sk.contains("extraDirs") && sk["extraDirs"].is_array()) {
            for (auto const& v : sk["extraDirs"]) {
                if (v.is_string() && !v.get<std::string>().empty()) mSkinExtraDirs.push_back(v.get<std::string>());
            }
        }
    }

    // PAPI 占位符格式
    mPapiEnabled = true;
    mPapiFormats.clear();
    if (mConfig.contains("papi") && mConfig["papi"].is_object()) {
        auto const& p = mConfig["papi"];
        mPapiEnabled = jbool(p, "enabled", true);
        if (p.contains("formats") && p["formats"].is_object()) {
            for (auto it = p["formats"].begin(); it != p["formats"].end(); ++it) {
                if (it.value().is_string()) mPapiFormats[it.key()] = it.value().get<std::string>();
            }
        }
    }

    // 标量
    // 取子对象引用（缺失/类型不符一律返回空对象, 不抛异常）
    static json const kEmptyObj = json::object();
    auto subOf = [](json const& parent, char const* k) -> json const& {
        if (!parent.is_object()) return kEmptyObj;
        auto it = parent.find(k);
        return (it != parent.end() && it->is_object()) ? *it : kEmptyObj;
    };
    auto const& econ  = subOf(mConfig, "economy");
    auto const& tp    = subOf(mConfig, "teleport");
    auto const& rally = subOf(mConfig, "rally");
    auto const& notif = subOf(mConfig, "notification");
    auto const& sound = subOf(notif, "sound");
    auto const& side  = subOf(notif, "sidebar");
    auto const& rtp   = subOf(mConfig, "randomTeleport");
    auto const& btp   = subOf(mConfig, "blockTeleport");
    auto const& quick = subOf(btp, "quickAdd");
    auto const& edit  = subOf(btp, "editTool");
    auto const& rules = subOf(mConfig, "defaultRules");
    auto const& log   = subOf(mConfig, "logging");

    mEconomyEnabled     = jbool(econ, "enabled", false);
    mEconomyType        = jstr(econ, "type", "llmoney");
    mScoreboardName     = jstr(econ, "scoreboardName", "money");
    mMoneyName          = jstr(econ, "moneyName", "金币");

    mCooldownTime       = jint(tp, "cooldownTime", 30);
    mEffectiveDuration  = jint(tp, "effectiveDuration", 60);
    mMaxPrivateWarps    = std::max(1, jint(tp, "maxPrivateWarps", 5));

    mRallyEnabled       = jbool(rally, "enabled", true);
    mRallyDuration      = jint(rally, "duration", 60);
    mRallyCooldownMs    = jint(rally, "cooldown", 60) * 1000;

    mSoundEnabled       = jbool(sound, "enabled", true);
    mSoundName          = jstr(sound, "soundName", "mob.endermen.portal");
    mSoundPitch         = jfloat(sound, "pitch", 1.0f);
    mSoundVolume        = jfloat(sound, "volume", 1.0f);
    mSidebarEnabled     = jbool(side, "enabled", true);
    mSidebarFormat      = jstr(side, "format", "§6[TPA] §e{player} §7{type}");

    mRandomEnabled      = jbool(rtp, "enabled", true);
    mRandomCooldown     = jint(rtp, "cooldownSeconds", 120);
    mRandomMaxAttempts  = jint(rtp, "maxAttempts", 50);
    mRandomDebug        = jbool(rtp, "debug", true);

    mBlockTpEnabled     = jbool(btp, "enabled", true);
    mBlockTpQuickAddItem           = jstr(quick, "item", "minecraft:nether_star");
    mBlockTpQuickAddRequireSneak   = jbool(quick, "requireSneak", true);
    mBlockTpQuickAddRequireAdmin   = jbool(quick, "requireAdmin", true);
    mBlockTpQuickAddCooldownMs     = jint(quick, "cooldownMs", 1000);
    mBlockTpFixedCooldownMs        = jint(btp, "fixedCooldownMs", 200);

    mEditToolEnabled        = jbool(edit, "enabled", true);
    mEditToolItem           = jstr(edit, "item", "minecraft:nether_star");
    mEditToolItemRequireSneak = jbool(edit, "itemRequiresSneak", false);
    mEditToolSneak          = jbool(edit, "sneak", true);
    mEditToolRequireAdmin   = jbool(edit, "requireAdmin", true);

    mDefaultRefuseAllTpa        = jbool(rules, "refuseAllTpaRequest", false);
    mDefaultNoPopUpWindow       = jbool(rules, "noPopUpWindow", false);
    mDefaultRefuseAllWarpRequests = jbool(rules, "refuseAllWarpRequests", false);
    mLogToConsole               = jbool(log, "logToConsole", true);
    mBroadcastToGame            = jbool(log, "broadcastToGame", true);
}

bool Config::load() {
    std::error_code ec;
    fs::create_directories(dataDir(), ec);
    fs::create_directories(npcSkinsDir(), ec);

    auto path = configPath();
    if (!fs::exists(path)) {
        mConfig = defaultConfig();
        buildCaches();
        save();
        return true;
    }
    try {
        std::ifstream f(path);
        f >> mConfig;
        f.close();
    } catch (std::exception const&) {
        mConfig = defaultConfig();
        buildCaches();
        return false;
    }
    // 老配置里已下线/已改名的东西在这里收拾（幂等）
    bool const migrated = migrateLegacy();
    // 缓存构建挪出 try: 单项类型不符只回落该默认值, 不会把整份配置重置成默认
    buildCaches();
    if (migrated) save();
    return true;
}

// 旧版默认指令名 → 现在的新默认名（指令名统一改短, rallyjoin 并进了 accept）
namespace {
constexpr std::pair<char const*, char const*> kLegacyCommandNames[] = {
    {"private",     "mywarp"},
    {"public",      "pubwarp"},
    {"noapproval",  "napwarp"},
    {"browser",     "pwarp"},
    {"rally",       "rally"},
    {"settings",    "tpset"},
    {"blacklist",   "tpblock"},
    {"admin",       "tpadmin"},
    {"blocktp",     "blocktp"},
    {"crossserver", "cswarp"},
};
} // namespace

bool Config::migrateLegacy() {
    bool changed = false;
    // 物理传送点模式已下线
    if (mConfig.erase("physicalWarpOnlyMode") > 0) changed = true;
    // 指令名: 只改"还是旧默认值"的项, 用户自己取的名字一概不动
    auto cmds = mConfig.find("commands");
    if (cmds != mConfig.end() && cmds->is_object()) {
        auto const defs = defaultConfig()["commands"];
        if (cmds->erase("rallyjoin") > 0) changed = true;
        for (auto const& [key, oldName] : kLegacyCommandNames) {
            auto it = cmds->find(key);
            if (it == cmds->end() || !it->is_string()) continue;
            if (it->get<std::string>() != oldName) continue;
            auto def = defs.find(key);
            if (def == defs.end()) continue;
            *it = *def;
            changed = true;
        }
        // 新增的指令键补进去, 否则它们在 Config.json 里看不见也就改不成
        for (auto it = defs.begin(); it != defs.end(); ++it) {
            if (!cmds->contains(it.key())) {
                (*cmds)[it.key()] = it.value();
                changed        = true;
            }
        }
    }
    // 皮肤来源配置段同理: 缺了就不扫 MHR 的皮肤目录（用户也看不到这项配置）
    if (!mConfig.contains("skins")) {
        mConfig["skins"] = defaultConfig()["skins"];
        changed          = true;
    }
    // 管理员编辑工具同理: 缺了功能照常（读的时候会用默认值）, 但配置里看不见就改不成
    auto btp = mConfig.find("blockTeleport");
    if (btp != mConfig.end() && btp->is_object() && !btp->contains("editTool")) {
        (*btp)["editTool"] = defaultConfig()["blockTeleport"]["editTool"];
        changed            = true;
    }
    return changed;
}

bool Config::save() {
    // 原子写: 先写 .tmp 再替换。配置被管理菜单频繁改写, 写一半崩溃会毁掉整份配置
    std::string const out = mConfig.dump(2);
    std::string const path = configPath().string();
    std::string const tmp  = path + ".tmp";
    try {
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            f << out;
            f.flush();
            if (!f.good()) { f.close(); return false; }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (!ec) return true;
    } catch (...) {}
    try {   // 退化: 直接覆盖写
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << out;
        return f.good();
    } catch (...) {
        return false;
    }
}

bool Config::update(std::function<void(json&)> const& fn) {
    try {
        fn(mConfig);
    } catch (...) {
        return false;
    }
    buildCaches();   // 立刻让 getter 看到新值（缓存是全量的, 不重建就会读到旧值）
    return save();
}

bool Config::reload() {
    return load();
}

bool Config::isFeatureEnabled(std::string const& key) const {
    auto it = mFeatureCache.find(key);
    return it != mFeatureCache.end() ? it->second : false;
}

std::string Config::getNotifyText(std::string const& key) const {
    auto it = mNotifyTextCache.find(key);
    return it != mNotifyTextCache.end() ? it->second : key;
}

int Config::getCost(std::string const& key) const {
    auto it = mCostCache.find(key);
    return it != mCostCache.end() ? it->second : 0;
}

std::string Config::getCommand(std::string const& key) const {
    // 缓存里没有 = 不认识的键, 返回空串（= 不注册该指令）, 不再拿键名当指令名
    auto it = mCommandCache.find(key);
    return it != mCommandCache.end() ? it->second : std::string{};
}

std::vector<std::string> const& Config::dangerBlocks() const { return mDangerBlocksCache; }
std::vector<std::string> const& Config::dangerShortBlocks() const { return mDangerShortCache; }
std::vector<RandomPreset> const& Config::randomPresets() const { return mPresetsCache; }

// 管理员参数写入
// 每个 setter: 校验键 → 改 mConfig → buildCaches() 刷新缓存 → save() 落盘。
// 配置是加载期缓存的, 所以运行时改配置必须同时更新 mConfig 与缓存, 统一走这里。

namespace {

json& ensureObject(json& parent, char const* key) {   // 取子对象; 缺失或类型不符则重建为空对象
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_object()) {
        parent[key] = json::object();
        return parent[key];
    }
    return *it;
}

bool keyIn(std::string const& key, std::initializer_list<char const*> allowed) {
    for (auto* k : allowed) {
        if (key == k) return true;
    }
    return false;
}

} // namespace

bool Config::setMaxPrivateWarps(int v) {
    ensureObject(mConfig, "teleport")["maxPrivateWarps"] = std::clamp(v, 1, 50);
    buildCaches();
    return save();
}

bool Config::setTeleportParams(int cooldownSec, int effectiveDurationSec) {
    auto& tp = ensureObject(mConfig, "teleport");
    tp["cooldownTime"]      = std::clamp(cooldownSec, 0, 300);
    tp["effectiveDuration"] = std::clamp(effectiveDurationSec, 10, 300);
    buildCaches();
    return save();
}

bool Config::setEconomyEnabled(bool on) {
    ensureObject(mConfig, "economy")["enabled"] = on;
    buildCaches();
    return save();
}

bool Config::setEconomyType(std::string const& type) {
    if (!keyIn(type, {"llmoney", "scoreboard"})) return false;
    ensureObject(mConfig, "economy")["type"] = type;
    buildCaches();
    return save();
}

bool Config::setEconomyText(std::string const& key, std::string const& value) {
    if (!keyIn(key, {"scoreboardName", "moneyName"})) return false;
    ensureObject(mConfig, "economy")[key] = value;
    buildCaches();
    return save();
}

bool Config::setEconomyCost(std::string const& key, int cost) {
    if (!keyIn(key, {"publicWarp", "privateWarp", "noApprovalWarp", "tpa", "rally", "randomTeleport"})) return false;
    ensureObject(ensureObject(mConfig, "economy"), "costs")[key] = std::clamp(cost, 0, 1000000);
    buildCaches();
    return save();
}

bool Config::setDefaultRule(std::string const& key, bool on) {
    if (!keyIn(key, {"refuseAllTpaRequest", "noPopUpWindow", "refuseAllWarpRequests"})) return false;
    ensureObject(mConfig, "defaultRules")[key] = on;
    buildCaches();
    return save();
}

bool Config::setLogging(std::string const& key, bool on) {
    if (!keyIn(key, {"logToConsole", "broadcastToGame"})) return false;
    ensureObject(mConfig, "logging")[key] = on;
    buildCaches();
    return save();
}

// PAPI 占位符: 先看配置里有没有显式写; 没写用内置默认; papi.enabled=false 一律不注册
std::string Config::papiFormat(std::string const& name) const {
    if (!mPapiEnabled) return {};
    auto it = mPapiFormats.find(name);
    if (it != mPapiFormats.end()) return it->second;   // 显式配置（空串 = 关掉这一条）

    static std::unordered_map<std::string, std::string> const kDefaults = {
        {"mtps_tpa",       "§6[TPA] §e{player} §7{type}"},
        {"mtps_tpa_count", "§e{count}"},
        {"mtps_warps",     "§e{count}§7/§a{max}"},
        {"mtps_shared",    "§e{count}"},
        {"mtps_public",    "§e{count}"},
    };
    auto d = kDefaults.find(name);
    return d != kDefaults.end() ? d->second : std::string{};
}

std::vector<std::string> const& Config::papiNames() {
    static std::vector<std::string> const kNames = {
        "mtps_tpa", "mtps_tpa_count", "mtps_warps", "mtps_shared", "mtps_public",
    };
    return kNames;
}

bool Config::setPresets(std::vector<RandomPreset> const& list) {
    json arr = json::array();
    for (auto& p : list) {
        arr.push_back({
            {"name", p.name},
            {"enabled", p.enabled},
            {"originMode", p.originMode},
            {"originX", p.originX},
            {"originZ", p.originZ},
            {"radius", p.radius},
            {"dimid", p.dimid},
            {"cooldown", p.cooldown},
            {"message", p.message},
            {"economy", {{"enabled", p.economy.enabled}, {"cost", p.economy.cost}, {"type", p.economy.type}}},
        });
    }
    ensureObject(mConfig, "randomTeleport")["presets"] = std::move(arr);
    buildCaches();
    return save();
}

std::vector<std::pair<std::string, std::string>> const& Config::costKeys() {
    static std::vector<std::pair<std::string, std::string>> const kKeys = {
        {"publicWarp",     "公共传送点"},
        {"privateWarp",    "私人传送点"},
        {"noApprovalWarp", "免申请传送点"},
        {"tpa",            "玩家互传"},
        {"rally",          "召集传送"},
        {"randomTeleport", "随机传送"},
    };
    return kKeys;
}

} // namespace mtps
