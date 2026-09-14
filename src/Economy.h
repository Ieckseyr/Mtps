#pragma once

#include <string>
#include <cstdint>

class Player;

namespace mtps {

class Economy {
public:
    static Economy& getInstance();

    // 查询余额
    bool getBalance(Player& player, int64_t& balanceOut);
    // 扣款
    bool withdraw(Player& player, int64_t cost);
    // 加款（退款）
    bool deposit(Player& player, int64_t amount);

    // 检查是否可负担
    bool canAfford(Player& player, int64_t cost);

    // 经济类型是否可用
    bool isAvailable();

    // 获取当前经济类型名称
    std::string getTypeName();

private:
    Economy() = default;

    using FnLLMoneyGet = long long (*)(std::string);
    using FnLLMoneySet = bool (*)(std::string, long long);
    using FnLLMoneyAdd = bool (*)(std::string, long long, std::string);

    void ensureLLMoney();
    void*        mMoneyDll = nullptr;
    FnLLMoneyGet mFnGet    = nullptr;
    FnLLMoneySet mFnSet    = nullptr;
    FnLLMoneyAdd mFnAdd    = nullptr;
    bool         mMoneyChecked = false;

    static bool getScore(Player& player, std::string const& objective, int& out);
    static bool addScore(Player& player, std::string const& objective, int amount);
};

} // namespace mtps
