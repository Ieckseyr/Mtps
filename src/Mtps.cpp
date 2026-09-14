// Mtps.cpp - 插件入口: 命令注册 / 事件监听 / NPC 交互路由 / tick 驱动
#include "Mtps.h"

#include "ArchiveScanner.h"
#include "Config.h"
#include "DataStore.h"
#include "Menu.h"
#include "Pinyin.h"
#include "NpcTeleport.h"
#include "NpcSkin.h"
#include "RandomTeleport.h"
#include "TpaRally.h"

#include <hologramlib/HologramLib.h>

#include "MtpsPapi.h"

#include <ll/api/command/CommandHandle.h>
#include <ll/api/command/CommandRegistrar.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/ListenerBase.h>
#include <ll/api/event/player/PlayerJoinEvent.h>
#include <ll/api/event/world/ServerLevelTickEvent.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/service/Bedrock.h>

#include <mc/server/commands/CommandOrigin.h>
#include <mc/server/commands/CommandOutput.h>
#include <mc/server/commands/CommandPermissionLevel.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>

#include <cctype>
#include <set>

// LeviLamina 26.40 事件 ID 对齐补丁: 26.40 把事件类挪进 inline namespace, 而 EventId 基于
// 类型名反射 —— Clang 省略 inline ns、MSVC 保留, 两侧 hash 不一致, MSVC 插件收不到任何
// ll::event 事件(RTP 无响应 / TPS 恒 0)。这里显式特化 getEventId 对齐 Clang 侧的名字。
namespace ll::event {
template <>
constexpr EventIdView getEventId<::ll::event::world::ServerLevelTickEvent> =
    EventIdView{"ll::event::ServerLevelTickEvent"};
} // namespace ll::event

namespace mtps {

using ll::command::CommandRegistrar;
using ll::command::CommandHandle;

namespace {

Player* playerOf(CommandOrigin const& origin) {
    auto* entity = origin.getEntity();
    return (entity && entity->isPlayer()) ? static_cast<Player*>(entity) : nullptr;
}

Player* checkOp(CommandOrigin const& origin, CommandOutput& output) {
    auto* player = playerOf(origin);
    if (!player) {
        output.error("§c需要玩家执行");
        return nullptr;
    }
    if ((int)player->getCommandPermissionLevel() < 1) {
        output.error("§c权限不足（需OP）");
        return nullptr;
    }
    return player;
}

// 命令参数结构（命名空间级, 避免局部 struct 触发 MSVC ICE）
struct ActionP {
    std::string action{""};
};

} // namespace

Mtps& Mtps::getInstance() {
    static Mtps instance;
    return instance;
}

bool Mtps::load() {
    // 配置 + 数据加载
    if (!Config::getInstance().load()) {
        getSelf().getLogger().error("配置加载失败");
        return false;
    }
    if (!DataStore::getInstance().loadAll()) {
        getSelf().getLogger().warn("部分数据文件加载失败（可能为首次运行）");
    }
    // 拼音表（可选）: Meowdata/Mtps/pinyin.txt, 供传送点模糊搜索按拼音/首字母匹配
    loadPinyinTable();
    if (pinyinTableLoaded()) {
        getSelf().getLogger().info("拼音表已加载, 传送点搜索支持拼音/首字母匹配");
    } else {
        getSelf().getLogger().info("未找到 Meowdata/Mtps/pinyin.txt, 传送点搜索的拼音匹配已跳过");
    }
    return true;
}

bool Mtps::enable() {
    // HologramLib 版本协商: 需要 1.19.1（多播 ghost 交互 + NPC 皮肤 blob API）
    constexpr uint32_t kRequired = 0x011901;
    auto const        ver        = hologramlib::IHologramLib::getInstance().version();
    if (ver < kRequired) {
        getSelf().getLogger().error("HologramLib 版本过低: 当前 0x{:06X}, 需要 >= 0x011901 (1.19.1)", ver);
        getSelf().getLogger().error("请将新版 HologramLib.dll 部署到 plugins/HologramLib/ 后重启服务器");
        return false;
    }

    // 皮肤: 先把 npc_skins 目录（以及 Config.skins.extraDirs, 默认含 MHR 的皮肤目录）
    // 扫一遍注册好 —— 后面创建 NPC 要按 skinId 去注册表里取, 取不到就创建不出来
    skins::refresh();

    // 创建 NPC 传送点（HologramLib IPlayerNpc + 常显悬浮字 + 皮肤 blob 恢复）
    NpcTeleport::getInstance().init();

    // Ghost 交互多播监听（NPC 域: 右键 NPC 触发传送）
    mGhostToken = hologramlib::IHologramLib::getInstance().addGhostInteractListener(
        [](hologramlib::GhostInteractEvent const& ev) {
            // 1=右键交互 2=左键攻击 4=交互更新（某些客户端蹲下右键发的是 4）;
            // 是否触发由点位开关与编辑工具决定, 都在 NpcTeleport 内判断
            if (ev.action != 1 && ev.action != 2 && ev.action != 4) return;
            // 两种载体（假玩家NPC / 虚假实体）走同一个入口, 内部自己按 id 路由
            NpcTeleport::getInstance().onInteract(ev.domain, ev.playerName, ev.id, ev.action);
        }
    );
    if (mGhostToken == 0) {
        getSelf().getLogger().warn("Ghost 交互多播监听注册失败, NPC 传送交互不可用");
    }

    // 命令注册: 名字全部取自 Config.commands（留空 = 不注册该指令, 与 JS 版一致）
    {
        using Perm = CommandPermissionLevel;
        auto& reg = CommandRegistrar::getInstance(false);
        auto& cfg = Config::getInstance();

        // 指令名会直接进命令解析器, 所以这里自己先校验一遍字符集; 不合法就跳过并告警
        auto cmdName = [this, &cfg](char const* key) -> std::string {
            std::string const n = cfg.getCommand(key);
            if (n.empty()) return {};
            bool ok = (std::isalpha((unsigned char)n[0]) != 0) || n[0] == '_';
            for (char c : n) {
                if (std::isalnum((unsigned char)c) == 0 && c != '_') { ok = false; break; }
            }
            if (!ok) {
                getSelf().getLogger().warn("commands.{} 的指令名 \"{}\" 不合法（只能用字母/数字/下划线）, 已跳过", key, n);
                return {};
            }
            if (n.size() > 4) {
                getSelf().getLogger().warn("指令 /{} (commands.{}) 超过 4 个字母, 建议改短", n, key);
            }
            return n;
        };
        // desc 里的 {cmd} 会换成 "/实际指令名", 免得提示文案写死名字
        // 重名直接跳过并告警: 指令名现在是用户可编辑的, 两个功能挤在同一个名字上会互相抢重载
        std::set<std::string> usedNames;
        auto mkcmd = [&](char const* key, std::string desc, Perm perm) -> CommandHandle* {
            std::string const n = cmdName(key);
            if (n.empty()) return nullptr;
            if (!usedNames.insert(n).second) {
                getSelf().getLogger().warn("指令名 /{} 被多个 commands.* 同时使用, 已跳过 commands.{}", n, key);
                return nullptr;
            }
            if (auto p = desc.find("{cmd}"); p != std::string::npos) desc.replace(p, 5, "/" + n);
            return &reg.getOrCreateCommand(n, desc, perm);
        };
        int registered = 0;

        // 主菜单
        if (auto* cmd = mkcmd("menu", "打开传送系统主菜单", Perm::Any)) {
            registered++;
            cmd->overload<ActionP>().optional("action").execute(
                [](CommandOrigin const& origin, CommandOutput& output, ActionP const&) {
                    auto* player = playerOf(origin);
                    if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                    menu::openMainMenu(*player);
                });
        }
        // 传送点系统
        if (auto* cmd = mkcmd("warp", "传送点系统", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openWarpMenu(*player);
            });
        }
        // 我的传送点
        if (auto* cmd = mkcmd("private", "我的传送点（可用: {cmd} 编号 直达）", Perm::Any)) {
            registered++;
            cmd->overload<ActionP>().optional("action").execute(
                [](CommandOrigin const& origin, CommandOutput& output, ActionP const& p) {
                    auto* player = playerOf(origin);
                    if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                    menu::openPrivateWarpMenu(*player, p.action);
                });
        }
        // 玩家互传
        if (auto* cmd = mkcmd("tpa", "玩家互传", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openTpaSelectForm(*player);
            });
        }
        // 随机传送
        if (auto* cmd = mkcmd("random", "随机传送（可用: {cmd} 编号 直达预设）", Perm::Any)) {
            registered++;
            cmd->overload<ActionP>().optional("action").execute(
                [](CommandOrigin const& origin, CommandOutput& output, ActionP const& p) {
                    auto* player = playerOf(origin);
                    if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                    menu::openRandomTeleportMenu(*player, p.action);
                });
        }
        // 公共传送点
        if (auto* cmd = mkcmd("public", "公共传送点（可用: {cmd} 编号 直达）", Perm::Any)) {
            registered++;
            cmd->overload<ActionP>().optional("action").execute(
                [](CommandOrigin const& origin, CommandOutput& output, ActionP const& p) {
                    auto* player = playerOf(origin);
                    if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                    menu::openPublicWarpQuickList(*player, p.action);
                });
        }
        // 免申请传送点
        if (auto* cmd = mkcmd("noapproval", "免申请传送点（可用: {cmd} 编号 直达）", Perm::Any)) {
            registered++;
            cmd->overload<ActionP>().optional("action").execute(
                [](CommandOrigin const& origin, CommandOutput& output, ActionP const& p) {
                    auto* player = playerOf(origin);
                    if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                    menu::openNoApprovalWarpsList(*player, p.action);
                });
        }

        // 发起召集（已在召集中的发起者再执行 = 取消）
        if (auto* cmd = mkcmd("rally", "发起/取消召集传送", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::startRally(*player);
            });
        }
        // 请求管理
        if (auto* cmd = mkcmd("requests", "请求管理", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openRequestManageForm(*player);
            });
        }
        // 个人设置
        if (auto* cmd = mkcmd("settings", "个人设置", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openPersonalSettingsForm(*player);
            });
        }
        // 黑名单管理
        if (auto* cmd = mkcmd("blacklist", "黑名单管理（名单里的玩家不能向我发起传送）", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openBlacklistForm(*player);
            });
        }
        // 同意 / 拒绝（同意也用于响应召集）
        if (auto* cmd = mkcmd("accept", "同意传送请求 / 加入召集", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::acceptTeleportRequest(*player);
            });
        }
        if (auto* cmd = mkcmd("refuse", "拒绝传送请求", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::refuseTeleportRequest(*player);
            });
        }
        // 重载配置与数据（控制台/后台也能执行）
        if (auto* cmd = mkcmd("reload", "Mtps: 重载配置与数据（控制台可用）", Perm::GameDirectors)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const&, CommandOutput& output) {
                DataStore::getInstance().saveAll();     // 先落盘, 免得丢掉还在合并窗口里的改动
                bool cfgOk  = Config::getInstance().reload();
                bool dataOk = DataStore::getInstance().loadAll();
                skins::refresh();                       // 皮肤目录可能新增了文件, 重新扫一遍
                papi::registerAll();                    // 格式可能变了, 重新注册占位符
                output.success(std::string("Mtps 重载完成: 配置=") + (cfgOk ? "成功" : "失败") +
                               " 数据=" + (dataOk ? "成功" : "失败") + " PAPI=" + papi::statusText());
            });
        }
        // 系统管理
        if (auto* cmd = mkcmd("admin", "Mtps 系统管理", Perm::GameDirectors)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openAdminSettingsMenu(*player);
            });
        }
        // NPC 传送点管理
        if (auto* cmd = mkcmd("blocktp", "NPC传送点管理", Perm::GameDirectors)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openBlockTpMenu(*player);
            });
        }
        // 玩家传送点浏览
        if (auto* cmd = mkcmd("browser", "玩家传送点浏览", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openPublicWarpBrowser(*player);
            });
        }
        // 跨服传送
        if (auto* cmd = mkcmd("crossserver", "跨服传送", Perm::Any)) {
            registered++;
            cmd->overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
                auto* player = playerOf(origin);
                if (!player) { output.error("§c此命令仅限玩家执行！"); return; }
                menu::openCrossServerList(*player);
            });
        }

        getSelf().getLogger().info("已注册 {} 个指令（名字来自 Config.commands, 留空即不注册）", registered);
    }

    // tick 驱动: 随机传送扫描 + TPA 超时清理 + 召集超时
    mTickListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerLevelTickEvent>(
        [this](ll::event::ServerLevelTickEvent const&) {
            static bool sFirstTick = true; // 首次触发日志: 验证事件 ID 补丁生效
            if (sFirstTick) {
                sFirstTick = false;
                getSelf().getLogger().info("tick 事件已触发（事件 ID 对齐补丁生效）");
            }
            RandomTeleport::getInstance().tick();
            NpcTeleport::getInstance().tick();   // 虚假实体"逐客户端看向自己"
            TpaRally::getInstance().tick();
            DataStore::getInstance().tick();   // 到期把脏数据文件合并落盘（每 3 秒最多一次）
        }
    );
    if (!mTickListener) {
        getSelf().getLogger().error("tick 监听器注册失败! 随机传送/TPA 超时清理将无法工作");
    }

    // RTP 存档直读: 后台线程建 .ldb 索引（大存档十几秒, 不卡启用; 就绪前 RTP 回退生成路径）
    ArchiveScanner::getInstance().startAsync();

    // PAPI 占位符: 走 MeowPAPI 的 C ABI 注册（不链接它的静态库）
    papi::registerAll();
    getSelf().getLogger().info("PAPI: {}", papi::statusText());

    getSelf().getLogger().info("Mtps C++ 版已启用（随机传送四级数据源: 内存/落点表/存档直读/区块视野生成）");
    mEnabled = true;
    return true;
}

bool Mtps::disable() {
    // 移除 ghost 多播监听
    if (mGhostToken != 0) {
        hologramlib::IHologramLib::getInstance().removeGhostInteractListener(mGhostToken);
        mGhostToken = 0;
    }
    if (mTickListener) {
        ll::event::EventBus::getInstance().removeListener(mTickListener);
        mTickListener = nullptr;
    }
    // 关服兜底: 悬停中的随机传送玩家送回原点（防止高空位置被存档）
    RandomTeleport::getInstance().stopAll();
    // 存档直读索引: 停后台线程 + 关闭文件句柄（必须在 stopAll 之后, 先停 RTP 查询再销毁 reader）
    ArchiveScanner::getInstance().shutdown();
    DataStore::getInstance().saveAll();
    // PAPI 反注册: ABI 提供了 UnregisterByPlugin, 卸载前摘掉回调, 避免指向已卸载的代码
    papi::unregisterAll();
    mEnabled = false;
    return true;
}

} // namespace mtps

LL_REGISTER_MOD(mtps::Mtps, mtps::Mtps::getInstance());
