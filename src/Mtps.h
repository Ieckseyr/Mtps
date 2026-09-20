#pragma once

#include <ll/api/mod/NativeMod.h>
#include <ll/api/event/ListenerBase.h>
#include <memory>
#include <string>

namespace mtps {

class Mtps {
public:
    static Mtps& getInstance();

    Mtps() : mSelf(*ll::mod::NativeMod::current()) {}
    ~Mtps() = default;

    Mtps(Mtps&&)                 = delete;
    Mtps(Mtps const&)            = delete;
    Mtps& operator=(Mtps&&)      = delete;
    Mtps& operator=(Mtps const&) = delete;

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();

    bool disable();

private:
    ll::mod::NativeMod& mSelf;

    bool enableInner();   // enable 的实际实现（外层负责兜异常）
    // 指令注册拆出来单独兜异常: 命令注册表没就绪/版本不匹配时不该让整个插件停用,
    // 失败就留条数=0, 由 tick 侧重试（服务器起来后注册表通常就好了）
    int  registerCommands();
    void retryRegisterCommands();

    bool mEnabled{false};
    int  mRegisteredCommands{0};   // 已注册指令条数
    int  mCommandRetryLeft{0};     // 指令注册还有几次重试机会
    int64_t mTicks{0};             // 主 tick 计数（重试节流用）

    // HologramLib ghost 交互多播 token
    uint64_t mGhostToken{0};
    // 事件监听
    ll::event::ListenerPtr mTickListener;
    ll::event::ListenerPtr mPlayerJoinListener;
};

} // namespace mtps
