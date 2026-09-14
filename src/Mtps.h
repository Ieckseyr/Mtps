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

    bool mEnabled{false};

    // HologramLib ghost 交互多播 token
    uint64_t mGhostToken{0};
    // 事件监听
    ll::event::ListenerPtr mTickListener;
    ll::event::ListenerPtr mPlayerJoinListener;
};

} // namespace mtps
