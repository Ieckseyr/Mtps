#include "Economy.h"
#include "Config.h"

#include <ll/api/service/Bedrock.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <mc/world/scores/Objective.h>
#include <mc/world/scores/PlayerScoreSetFunction.h>
#include <mc/world/scores/Scoreboard.h>
#include <mc/world/scores/ScoreboardId.h>
#include <mc/world/scores/ScoreInfo.h>
#include <mc/world/scores/ScoreboardOperationResult.h>

#include <windows.h>

namespace mtps {

Economy& Economy::getInstance() {
    static Economy instance;
    return instance;
}

void Economy::ensureLLMoney() {
    if (mMoneyChecked) return;
    mMoneyChecked = true;
    HMODULE hMod = GetModuleHandleA("LegacyMoney.dll");
    if (!hMod) {
        hMod = static_cast<HMODULE>(mMoneyDll = LoadLibraryA("LegacyMoney.dll"));
    }
    if (hMod) {
        mFnGet = reinterpret_cast<FnLLMoneyGet>(GetProcAddress(hMod, "LLMoney_Get"));
        mFnSet = reinterpret_cast<FnLLMoneySet>(GetProcAddress(hMod, "LLMoney_Set"));
        mFnAdd = reinterpret_cast<FnLLMoneyAdd>(GetProcAddress(hMod, "LLMoney_Add"));
        if (!(mFnGet && mFnSet)) {
            mFnGet = nullptr; mFnSet = nullptr; mFnAdd = nullptr;
        }
    }
}

bool Economy::getScore(Player& player, std::string const& objective, int& out) {
    try {
        auto level = ll::service::getLevel();
        if (!level.has_value()) return false;
        Scoreboard& sb = level->getScoreboard();
        Objective* obj = sb.getObjective(objective);
        if (!obj) { out = 0; return true; }
        ScoreboardId const& id = sb.getScoreboardId(player);
        ScoreInfo info = obj->getPlayerScore(id);
        out = info.mValid ? info.mValue : 0;
        return true;
    } catch (...) { return false; }
}

bool Economy::addScore(Player& player, std::string const& objective, int amount) {
    try {
        auto level = ll::service::getLevel();
        if (!level.has_value()) return false;
        Scoreboard& sb = level->getScoreboard();
        Objective* obj = sb.getObjective(objective);
        if (!obj) return false;
        ScoreboardId const& id = sb.getScoreboardId(player);
        ScoreboardOperationResult result{};
        auto fn = amount >= 0 ? PlayerScoreSetFunction::Add : PlayerScoreSetFunction::Subtract;
        sb.modifyPlayerScore(result, id, *obj, std::abs(amount), fn);
        return true;
    } catch (...) { return false; }
}

bool Economy::isAvailable() {
    if (!Config::getInstance().economyEnabled()) return false;
    std::string type = Config::getInstance().economyType();
    if (type == "llmoney") {
        ensureLLMoney();
        return mFnGet && mFnSet;
    }
    if (type == "scoreboard") return true;
    return false;
}

std::string Economy::getTypeName() {
    return Config::getInstance().economyType();
}

bool Economy::getBalance(Player& player, int64_t& balanceOut) {
    std::string type = Config::getInstance().economyType();
    if (type == "llmoney") {
        ensureLLMoney();
        if (!mFnGet) return false;
        try { balanceOut = mFnGet(player.getXuid()); return true; } catch (...) { return false; }
    }
    if (type == "scoreboard") {
        int v = 0;
        if (!getScore(player, Config::getInstance().scoreboardName(), v)) return false;
        balanceOut = v;
        return true;
    }
    return false;
}

bool Economy::withdraw(Player& player, int64_t cost) {
    if (cost <= 0) return true;
    std::string type = Config::getInstance().economyType();
    if (type == "llmoney") {
        ensureLLMoney();
        if (!mFnGet || !mFnSet) return false;
        try {
            long long balance = mFnGet(player.getXuid());
            if (balance < cost) return false;
            return mFnSet(player.getXuid(), balance - cost);
        } catch (...) { return false; }
    }
    if (type == "scoreboard") {
        int v = 0;
        if (!getScore(player, Config::getInstance().scoreboardName(), v)) return false;
        if (v < cost) return false;
        return addScore(player, Config::getInstance().scoreboardName(), -(int)cost);
    }
    return false;
}

bool Economy::deposit(Player& player, int64_t amount) {
    if (amount <= 0) return true;
    std::string type = Config::getInstance().economyType();
    if (type == "llmoney") {
        ensureLLMoney();
        if (mFnAdd) {
            try { return mFnAdd(player.getXuid(), amount, "mtps"); } catch (...) {}
        }
        if (mFnGet && mFnSet) {
            try {
                long long balance = mFnGet(player.getXuid());
                return mFnSet(player.getXuid(), balance + amount);
            } catch (...) { return false; }
        }
        return false;
    }
    if (type == "scoreboard") {
        return addScore(player, Config::getInstance().scoreboardName(), (int)amount);
    }
    return false;
}

bool Economy::canAfford(Player& player, int64_t cost) {
    if (cost <= 0) return true;
    int64_t balance = 0;
    if (!getBalance(player, balance)) return false;
    return balance >= cost;
}

} // namespace mtps
