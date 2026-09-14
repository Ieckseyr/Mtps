#pragma once

#include "DataTypes.h"
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <functional>

namespace mtps {

namespace fs = std::filesystem;
using json = nlohmann::json;

class Config {
public:
    static Config& getInstance();

    bool load();
    bool save();
    bool reload();

    // 就地修改配置（管理菜单用）: fn 拿到 json 引用自行改, 之后自动重建缓存 + 原子落盘
    bool update(std::function<void(json&)> const& fn);
    // 只读访问原始 json（表单展示"当前值"用; 不要在外层保存它）
    json const& raw() const { return mConfig; }

    // 数据目录
    static fs::path const& dataDir();
    static fs::path configPath();
    static fs::path privateWarpsPath();
    static fs::path publicWarpsPath();
    static fs::path tpaCachePath();
    static fs::path personalRulesPath();
    static fs::path warpRequestsPath();
    static fs::path blockTpPointsPath();
    static fs::path crossServerPath();
    static fs::path npcSkinsDir();

    // 功能开关
    bool isFeatureEnabled(std::string const& key) const;
    // 全部功能开关（一次遍历取用, 免多次 map 查找）
    std::unordered_map<std::string, bool> const& featureFlags() const { return mFeatureCache; }

    // 经济
    bool                    economyEnabled() const { return mEconomyEnabled; }
    std::string const&      economyType() const { return mEconomyType; }
    std::string const&      scoreboardName() const { return mScoreboardName; }
    std::string const&      moneyName() const { return mMoneyName; }
    int                     getCost(std::string const& key) const;

    // 传送
    int  cooldownTime() const { return mCooldownTime; }
    int  effectiveDuration() const { return mEffectiveDuration; }
    int  maxPrivateWarps() const { return mMaxPrivateWarps; }

    // 召集
    bool        rallyEnabled() const { return mRallyEnabled; }
    int         rallyDuration() const { return mRallyDuration; }
    int         rallyCooldownMs() const { return mRallyCooldownMs; }

    // 通知
    bool               soundEnabled() const { return mSoundEnabled; }
    std::string const& soundName() const { return mSoundName; }
    float              soundPitch() const { return mSoundPitch; }
    float              soundVolume() const { return mSoundVolume; }
    bool               sidebarEnabled() const { return mSidebarEnabled; }
    std::string const& sidebarFormat() const { return mSidebarFormat; }
    std::string        getNotifyText(std::string const& key) const;

    // 随机传送
    bool        randomTeleportEnabled() const { return mRandomEnabled; }
    int         randomCooldownSeconds() const { return mRandomCooldown; }
    int         randomMaxAttempts() const { return mRandomMaxAttempts; }
    bool        randomDebug() const { return mRandomDebug; }
    std::vector<std::string> const& dangerBlocks() const;
    // 危险方块短名（已剥离 minecraft: 前缀, 存档 palette 用）
    std::vector<std::string> const& dangerShortBlocks() const;
    std::vector<RandomPreset> const& randomPresets() const;

    // 方块传送点
    bool               blockTpEnabled() const { return mBlockTpEnabled; }
    std::string const& blockTpQuickAddItem() const { return mBlockTpQuickAddItem; }
    bool               blockTpQuickAddRequireSneak() const { return mBlockTpQuickAddRequireSneak; }
    bool               blockTpQuickAddRequireAdmin() const { return mBlockTpQuickAddRequireAdmin; }
    int                blockTpQuickAddCooldownMs() const { return mBlockTpQuickAddCooldownMs; }
    int                blockTpFixedCooldownMs() const { return mBlockTpFixedCooldownMs; }

    // 管理员编辑工具: 手持编辑物品 / 蹲下 + 右键 NPC·虚假实体 → 直接打开该点的管理表单
    bool               editToolEnabled() const { return mEditToolEnabled; }
    std::string const& editToolItem() const { return mEditToolItem; }          // 空串 = 不启用物品方式
    bool               editToolItemRequiresSneak() const { return mEditToolItemRequireSneak; }
    bool               editToolSneak() const { return mEditToolSneak; }        // 蹲下右键也算
    bool               editToolRequireAdmin() const { return mEditToolRequireAdmin; }

    // 命令
    std::string getCommand(std::string const& key) const;

    // NPC 皮肤的额外来源目录（本插件自己的皮肤目录固定是 npcSkinsDir(), 不在这个表里）
    std::vector<std::string> const& skinExtraDirs() const { return mSkinExtraDirs; }

    // 默认规则
    bool defaultRefuseAllTpa() const { return mDefaultRefuseAllTpa; }
    bool defaultNoPopUpWindow() const { return mDefaultNoPopUpWindow; }
    bool defaultRefuseAllWarpRequests() const { return mDefaultRefuseAllWarpRequests; }

    // 日志
    bool logToConsole() const { return mLogToConsole; }
    bool broadcastToGame() const { return mBroadcastToGame; }

    // 危险方块集合（缓存）
    std::vector<std::string> const& getDangerBlocks() { return mDangerBlocksCache; }

    // 管理员参数写入（改内存配置 + 立即落盘 + 刷新缓存）
    // 配置改为加载期缓存后, 运行时改配置必须同时更新 mConfig 与缓存, 所以统一走这里
    bool setMaxPrivateWarps(int v);                                       // 1..50
    bool setTeleportParams(int cooldownSec, int effectiveDurationSec);    // 0..300 / 10..300
    bool setEconomyEnabled(bool on);
    bool setEconomyType(std::string const& type);                         // llmoney / scoreboard
    bool setEconomyText(std::string const& key, std::string const& value);// scoreboardName / moneyName
    bool setEconomyCost(std::string const& key, int cost);                // costs.* 六项
    bool setDefaultRule(std::string const& key, bool on);                 // defaultRules 三项
    bool setLogging(std::string const& key, bool on);                     // logging 两项
    // 整表覆盖随机传送预设（管理菜单增删改后调用）
    bool setPresets(std::vector<RandomPreset> const& presets);
    // 费用项键名 + 显示名（管理菜单表单用）
    static std::vector<std::pair<std::string, std::string>> const& costKeys();

    // PAPI 占位符: 名字 -> 文本格式（空串 = 不注册该占位符）
    // 可在 Config.json 的 papi.formats 里改; papi.enabled 为 false 时全部不注册
    std::string papiFormat(std::string const& name) const;
    static std::vector<std::string> const& papiNames();

private:
    Config() = default;
    json mConfig;

    // 加载期一次性解析的配置缓存
    // （原实现每次 getter 都用 "_json_pointer" 现场解析: 该字面量 UDL 非编译期,
    //   每次调用都要切分 token 串并分配 vector —— 在热路径上反复触发）
    std::vector<std::string> mDangerBlocksCache;
    std::vector<std::string> mDangerShortCache;
    std::vector<RandomPreset> mPresetsCache;
    std::unordered_map<std::string, bool>        mFeatureCache;
    std::unordered_map<std::string, int>         mCostCache;
    std::unordered_map<std::string, std::string> mNotifyTextCache;
    std::unordered_map<std::string, std::string> mCommandCache;
    std::vector<std::string>                     mSkinExtraDirs;
    bool mPapiEnabled{true};
    std::unordered_map<std::string, std::string> mPapiFormats;   // 仅存配置里显式写了的

    bool mEconomyEnabled{false};
    std::string mEconomyType{"llmoney"};
    std::string mScoreboardName{"money"};
    std::string mMoneyName{"金币"};
    int  mCooldownTime{30};
    int  mEffectiveDuration{60};
    int  mMaxPrivateWarps{5};
    bool mRallyEnabled{true};
    int  mRallyDuration{60};
    int  mRallyCooldownMs{60000};
    bool mSoundEnabled{true};
    std::string mSoundName{"mob.endermen.portal"};
    float mSoundPitch{1.0f};
    float mSoundVolume{1.0f};
    bool mSidebarEnabled{true};
    std::string mSidebarFormat{"§6[TPA] §e{player} §7{type}"};
    bool mRandomEnabled{true};
    int  mRandomCooldown{120};
    int  mRandomMaxAttempts{50};
    bool mRandomDebug{true};
    bool mBlockTpEnabled{true};
    std::string mBlockTpQuickAddItem{"minecraft:nether_star"};
    bool mBlockTpQuickAddRequireSneak{true};
    bool mBlockTpQuickAddRequireAdmin{true};
    int  mBlockTpQuickAddCooldownMs{1000};
    int  mBlockTpFixedCooldownMs{200};
    bool mEditToolEnabled{true};
    std::string mEditToolItem{"minecraft:nether_star"};
    bool mEditToolItemRequireSneak{false};
    bool mEditToolSneak{true};
    bool mEditToolRequireAdmin{true};
    bool mDefaultRefuseAllTpa{false};
    bool mDefaultNoPopUpWindow{false};
    bool mDefaultRefuseAllWarpRequests{false};
    bool mLogToConsole{true};
    bool mBroadcastToGame{true};

    void buildCaches();
    json defaultConfig() const;
    // 老配置的兼容收拾（下线项、改名的指令）, 返回是否真的改了东西
    bool migrateLegacy();
};

} // namespace mtps
