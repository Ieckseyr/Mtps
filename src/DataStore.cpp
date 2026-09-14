#include "DataStore.h"
#include "Config.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace mtps {

namespace {

// 合并落盘间隔（毫秒）: 期间内的多次改动合并成一次写盘
constexpr int64_t kFlushIntervalMs = 3000;

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

DataStore& DataStore::getInstance() {
    static DataStore instance;
    return instance;
}

json DataStore::loadJson(std::string const& path, json const& def) {
    std::ifstream f(path);
    if (!f.is_open()) return def;
    try {
        json j;
        f >> j;
        return j;
    } catch (...) {
        return def;
    }
}

// 原子写: 先写 <path>.tmp 再 rename 覆盖
// （直接覆盖写的窗口里崩溃/断电会留下半截 JSON, 下次加载解析失败 → 整份数据静默回退成默认值）
bool DataStore::saveJson(std::string const& path, json const& data) {
    std::string const out = data.dump(2);

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f.is_open()) {
            f << out;
            f.flush();
            if (f.good()) {
                f.close();
                std::error_code ec;
                std::filesystem::rename(tmp, path, ec);
                if (!ec) return true;
            }
            f.close();
        }
    }
    // 退化路径: rename 不可用（跨卷/被杀软占用）时退回直接覆盖写
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << out;
        return f.good();
    }
}

static WarpPos posFromJson(json const& j) {
    WarpPos p;
    if (j.is_array() && j.size() >= 4) {
        p.x = j[0].get<double>(); p.y = j[1].get<double>();
        p.z = j[2].get<double>(); p.dimid = j[3].get<int>();
    } else if (j.is_object()) {
        p.x = j.value("x", 0.0); p.y = j.value("y", 0.0);
        p.z = j.value("z", 0.0); p.dimid = j.value("dimid", 0);
    }
    return p;
}

static json posToJson(WarpPos const& p) {
    return json::array({p.x, p.y, p.z, p.dimid});
}

static EconomyEntry econFromJson(json const& j) {
    EconomyEntry e;
    e.enabled = j.value("enabled", false);
    e.type = j.value("type", "default");
    e.cost = j.value("cost", 0);
    return e;
}

static json econToJson(EconomyEntry const& e) {
    return json{{"enabled", e.enabled}, {"type", e.type}, {"cost", e.cost}};
}

bool DataStore::loadAll() {
    std::lock_guard<std::mutex> lock(mMutex);

    // 私人传送点（JS 版格式: { "xuid": { "private": [ {...} ] } }）
    auto pw = loadJson(Config::privateWarpsPath().string(), json::object());
    mPrivateWarps.clear();
    for (auto it = pw.begin(); it != pw.end(); ++it) {
        std::string xuid = it.key();
        // 兼容 { private: [...] } 包装 与 直接数组 两种格式
        json const* arrPtr = nullptr;
        if (it.value().is_object() && it.value().contains("private") && it.value()["private"].is_array()) {
            arrPtr = &it.value()["private"];
        } else if (it.value().is_array()) {
            arrPtr = &it.value();
        }
        if (!arrPtr) continue;
        json const& arr = *arrPtr;
        std::vector<PrivateWarp> warps;
        warps.reserve(arr.size());
        for (auto& w : arr) {
            PrivateWarp p;
            p.name = w.value("name", "");
            p.pos = posFromJson(w.value("pos", json::array({0,64,0,0})));
            p.createdAt = w.value("createdAt", (int64_t)0);
            p.noApproval = w.value("noApproval", false);
            if (w.contains("economy")) p.economy = econFromJson(w["economy"]);
            warps.push_back(std::move(p));
        }
        mPrivateWarps[xuid] = std::move(warps);
    }

    // 公共传送点
    auto pub = loadJson(Config::publicWarpsPath().string(), json::array());
    mPublicWarps.clear();
    mPublicWarps.reserve(pub.size());
    for (auto& w : pub) {
        PublicWarp p;
        p.name = w.value("name", "");
        p.pos = posFromJson(w.value("pos", json::array({0,64,0,0})));
        p.owner = w.contains("createdBy") ? w.value("createdBy", "") : w.value("owner", "");
        p.enabled = w.value("enabled", true);
        if (w.contains("economy")) p.economy = econFromJson(w["economy"]);
        mPublicWarps.push_back(std::move(p));
    }

    // btp 传送点
    auto bt = loadJson(Config::blockTpPointsPath().string(), json::array());
    mBlockTpPoints.clear();
    mBlockTpPoints.reserve(bt.size());
    for (auto& b : bt) {
        BlockTpPoint p;
        p.name = b.value("name", "");
        p.enabled = b.value("enabled", true);
        p.trigger = posFromJson(b.value("trigger", json::array({0,64,0,0})));
        p.isRandom = b.value("isRandom", false);
        p.target = posFromJson(b.value("target", json::array({0,64,0,0})));
        p.randomOriginX = b.value("randomOriginX", 0.0);
        p.randomOriginZ = b.value("randomOriginZ", 0.0);
        p.randomRadius = b.value("randomRadius", 1000);
        p.randomDimid = b.value("randomDimid", 0);
        p.randomOriginMode = b.value("randomOriginMode", "fixed");
        p.message = b.value("message", "");
        if (b.contains("itemFilter")) {
            for (auto& it : b["itemFilter"]) p.itemFilter.push_back(it.get<std::string>());
        }
        // 兼容 JS 版嵌套格式 floatingText: { enabled, text, offsetY }
        if (b.contains("floatingText") && b["floatingText"].is_object()) {
            auto& ft = b["floatingText"];
            p.floatingTextEnabled = ft.value("enabled", true);
            p.floatingText = ft.value("text", "");
            p.floatingOffsetY = ft.value("offsetY", 2.0f);
        } else {
            p.floatingTextEnabled = b.value("floatingTextEnabled", true);
            p.floatingText = b.value("floatingText", "");
            p.floatingOffsetY = b.value("floatingOffsetY", 2.0f);
        }
        // 兼容 JS 版嵌套随机范围 randomRange: { minX, maxX, minZ, maxZ, dimid }
        if (b.contains("randomRange") && b["randomRange"].is_object()) {
            auto& rr = b["randomRange"];
            double minX = rr.value("minX", 0.0), maxX = rr.value("maxX", 0.0);
            double minZ = rr.value("minZ", 0.0), maxZ = rr.value("maxZ", 0.0);
            p.randomOriginX = (minX + maxX) / 2.0;
            p.randomOriginZ = (minZ + maxZ) / 2.0;
            int dx = (int)(maxX - minX), dz = (int)(maxZ - minZ);
            p.randomRadius = std::max(std::max(dx, dz) / 2, 16);
            p.randomDimid = rr.value("dimid", 0);
        }
        if (b.contains("economy")) p.economy = econFromJson(b["economy"]);
        p.lookAtPlayers     = b.value("lookAtPlayers", true);
        p.triggerOnInteract = b.value("triggerOnInteract", true);
        p.triggerOnAttack   = b.value("triggerOnAttack", true);
        p.cooldownSeconds = b.value("cooldownSeconds", 0);
        p.carrier = b.value("carrier", 0);
        p.entityType = b.value("entityType", "minecraft:armor_stand");
        p.skinId = b.value("skinId", "");
        // 这三个字段以前叫 npcName/npcYaw/npcScale（其实两种载体共用）, 改名后读新键、回退旧键,
        // 老存档照常加载; 下一次落盘就会写成新键。
        p.displayName = b.contains("displayName") ? b.value("displayName", std::string{})
                                                  : b.value("npcName", std::string{});
        p.yaw         = b.contains("yaw") ? b.value("yaw", 0.0f) : b.value("npcYaw", 0.0f);
        p.scale       = b.contains("scale") ? b.value("scale", 1.0f) : b.value("npcScale", 1.0f);
        p.npcHeightOffset = b.value("npcHeightOffset", 0.0f);
        p.entityHeightOffset = b.value("entityHeightOffset", 0.0f);
        p.posOffsetX = b.value("posOffsetX", 0.0f);
        p.posOffsetZ = b.value("posOffsetZ", 0.0f);
        // 旧版还有个共用的 posOffsetY（与"高度微调"效果重复）: 老数据里若用过就折进两个高度微调
        if (b.contains("posOffsetY")) {
            float const legacyY = b.value("posOffsetY", 0.0f);
            if (legacyY != 0.0f) {
                p.npcHeightOffset += legacyY;
                p.entityHeightOffset += legacyY;
            }
        }
        mBlockTpPoints.push_back(std::move(p));
    }

    // TPA 缓存
    auto tc = loadJson(Config::tpaCachePath().string(), json::object());
    mTpaRequesters.clear();
    if (tc.contains("requesters")) {
        for (auto& r : tc["requesters"]) mTpaRequesters.push_back(r.get<std::string>());
    }

    // 传送点申请
    auto wr = loadJson(Config::warpRequestsPath().string(), json::array());
    mWarpRequests.clear();
    mWarpRequests.reserve(wr.size());
    for (auto& r : wr) {
        WarpRequest req;
        req.requester = r.value("requester", "");
        req.targetPlayer = r.value("targetPlayer", "");
        req.warpName = r.value("warpName", "");
        req.pos = posFromJson(r.value("pos", json::array({0,64,0,0})));
        req.timestamp = r.value("timestamp", (int64_t)0);
        req.duration = r.value("duration", 60);
        mWarpRequests.push_back(std::move(req));
    }

    // 跨服传送目的地
    auto cs = loadJson(Config::crossServerPath().string(), json::array());
    mCrossServers.clear();
    mCrossServers.reserve(cs.size());
    for (auto& c : cs) {
        CrossServerEntry e;
        e.name    = c.value("name", "");
        e.address = c.value("address", "");
        e.port    = c.value("port", 19132);
        e.enabled = c.value("enabled", true);
        if (!e.name.empty() && !e.address.empty()) mCrossServers.push_back(std::move(e));
    }

    // 个人规则
    auto pr = loadJson(Config::personalRulesPath().string(), json::object());
    mPersonalRules.clear();
    for (auto it = pr.begin(); it != pr.end(); ++it) {
        PersonalRules r;
        r.refuseAllTpaRequest = it.value().value("refuseAllTpaRequest", false);
        r.noPopUpWindow = it.value().value("noPopUpWindow", false);
        r.refuseAllWarpRequests = it.value().value("refuseAllWarpRequests", false);
        // 兼容旧键名 BlockedPlayers（JS 版用的）
        auto const& blk = it.value().contains("blockedPlayers") ? it.value()["blockedPlayers"]
                        : it.value().contains("BlockedPlayers") ? it.value()["BlockedPlayers"]
                                                               : json::array();
        if (blk.is_array()) {
            for (auto& n : blk) {
                if (n.is_string()) r.blockedPlayers.push_back(n.get<std::string>());
            }
        }
        mPersonalRules[it.key()] = r;
    }

    mDirty = 0;
    return true;
}

void DataStore::markDirtyLocked(uint32_t bits) {
    mDirty |= bits;
}

// 只写脏文件（tick 合并落盘走这里; 已落盘的文件不再重复序列化）
bool DataStore::flushLocked() {
    if (mDirty == 0) return true;
    uint32_t const bits = mDirty;
    mDirty = 0;

    if (bits & F_Private) {
        json pw = json::object();
        for (auto& [xuid, warps] : mPrivateWarps) {
            json arr = json::array();
            for (auto& w : warps) {
                arr.push_back({
                    {"name", w.name}, {"pos", posToJson(w.pos)},
                    {"createdAt", w.createdAt}, {"noApproval", w.noApproval},
                    {"economy", econToJson(w.economy)}
                });
            }
            pw[xuid] = json{ { "private", arr } };
        }
        if (!saveJson(Config::privateWarpsPath().string(), pw)) mDirty |= F_Private;    // 写失败则下次 tick 重试
    }

    if (bits & F_Public) {
        json pub = json::array();
        for (auto& w : mPublicWarps) {
            pub.push_back({
                {"name", w.name}, {"pos", posToJson(w.pos)},
                {"owner", w.owner}, {"enabled", w.enabled},
                {"economy", econToJson(w.economy)}
            });
        }
        if (!saveJson(Config::publicWarpsPath().string(), pub)) mDirty |= F_Public;    // 写失败则下次 tick 重试
    }

    if (bits & F_BlockTp) {
        json bt = json::array();
        for (auto& b : mBlockTpPoints) {
            bt.push_back({
                {"name", b.name}, {"enabled", b.enabled},
                {"trigger", posToJson(b.trigger)}, {"isRandom", b.isRandom},
                {"target", posToJson(b.target)},
                {"randomOriginX", b.randomOriginX}, {"randomOriginZ", b.randomOriginZ},
                {"randomRadius", b.randomRadius}, {"randomDimid", b.randomDimid},
                {"randomOriginMode", b.randomOriginMode},
                {"message", b.message}, {"itemFilter", b.itemFilter},
                {"floatingTextEnabled", b.floatingTextEnabled},
                {"floatingText", b.floatingText}, {"floatingOffsetY", b.floatingOffsetY},
                {"economy", econToJson(b.economy)},
                {"cooldownSeconds", b.cooldownSeconds},
                {"carrier", b.carrier}, {"entityType", b.entityType},
                {"lookAtPlayers", b.lookAtPlayers},
                {"triggerOnInteract", b.triggerOnInteract},
                {"triggerOnAttack", b.triggerOnAttack},
                {"skinId", b.skinId}, {"displayName", b.displayName},
                {"yaw", b.yaw}, {"scale", b.scale},
                {"npcHeightOffset", b.npcHeightOffset},
                {"entityHeightOffset", b.entityHeightOffset},
                {"posOffsetX", b.posOffsetX},
                {"posOffsetZ", b.posOffsetZ}
            });
        }
        if (!saveJson(Config::blockTpPointsPath().string(), bt)) mDirty |= F_BlockTp;    // 写失败则下次 tick 重试
    }

    if (bits & F_TpaCache) {
        json tc = {{"requesters", mTpaRequesters}, {"targets", json::array()}};
        if (!saveJson(Config::tpaCachePath().string(), tc)) mDirty |= F_TpaCache;    // 写失败则下次 tick 重试
    }

    if (bits & F_WarpReq) {
        json wr = json::array();
        for (auto& r : mWarpRequests) {
            wr.push_back({
                {"requester", r.requester}, {"targetPlayer", r.targetPlayer},
                {"warpName", r.warpName}, {"pos", posToJson(r.pos)},
                {"timestamp", r.timestamp}, {"duration", r.duration}
            });
        }
        if (!saveJson(Config::warpRequestsPath().string(), wr)) mDirty |= F_WarpReq;    // 写失败则下次 tick 重试
    }

    if (bits & F_Cross) {
        json cs = json::array();
        for (auto& c : mCrossServers) {
            cs.push_back({{"name", c.name}, {"address", c.address}, {"port", c.port}, {"enabled", c.enabled}});
        }
        if (!saveJson(Config::crossServerPath().string(), cs)) mDirty |= F_Cross;
    }

    if (bits & F_Rules) {
        json pr = json::object();
        for (auto& [xuid, r] : mPersonalRules) {
            pr[xuid] = {
                {"refuseAllTpaRequest", r.refuseAllTpaRequest},
                {"noPopUpWindow", r.noPopUpWindow},
                {"refuseAllWarpRequests", r.refuseAllWarpRequests},
                {"blockedPlayers", r.blockedPlayers}
            };
        }
        if (!saveJson(Config::personalRulesPath().string(), pr)) mDirty |= F_Rules;    // 写失败则下次 tick 重试
    }

    mLastFlushMs = nowMs();
    return true;
}

bool DataStore::saveAll() {
    std::lock_guard<std::mutex> lock(mMutex);
    mDirty = F_All;   // 显式调用语义: 全部文件都写一遍（与旧行为一致）
    return flushLocked();
}

void DataStore::tick() {
    if (mDirty == 0) return;                                  // 无锁快路径
    if (nowMs() - mLastFlushMs < kFlushIntervalMs) return;     // 未到合并窗口
    std::lock_guard<std::mutex> lock(mMutex);
    flushLocked();
}

// 私人传送点
std::vector<PrivateWarp> const& DataStore::getPrivateWarps(std::string const& xuid) const {
    static std::vector<PrivateWarp> const empty;
    auto it = mPrivateWarps.find(xuid);
    return it != mPrivateWarps.end() ? it->second : empty;
}

bool DataStore::addPrivateWarp(std::string const& xuid, PrivateWarp const& warp) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto& warps = mPrivateWarps[xuid];
    for (auto& w : warps) if (w.name == warp.name) return false;
    warps.push_back(warp);
    markDirtyLocked(F_Private);
    return true;
}

bool DataStore::removePrivateWarp(std::string const& xuid, std::string const& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mPrivateWarps.find(xuid);
    if (it == mPrivateWarps.end()) return false;
    auto& warps = it->second;
    auto wit = std::find_if(warps.begin(), warps.end(), [&](PrivateWarp const& w) { return w.name == name; });
    if (wit == warps.end()) return false;
    warps.erase(wit);
    markDirtyLocked(F_Private);
    return true;
}

bool DataStore::updatePrivateWarpPos(std::string const& xuid, std::string const& name, WarpPos const& pos) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mPrivateWarps.find(xuid);
    if (it == mPrivateWarps.end()) return false;
    for (auto& w : it->second) {
        if (w.name == name) { w.pos = pos; markDirtyLocked(F_Private); return true; }
    }
    return false;
}

bool DataStore::mutatePrivateWarp(std::string const& xuid, std::string const& name,
                                  std::function<void(PrivateWarp&)> const& fn) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mPrivateWarps.find(xuid);
    if (it == mPrivateWarps.end()) return false;
    for (auto& w : it->second) {
        if (w.name == name) { fn(w); markDirtyLocked(F_Private); return true; }
    }
    return false;
}

int DataStore::getPrivateWarpCount(std::string const& xuid) const {
    auto it = mPrivateWarps.find(xuid);
    return it != mPrivateWarps.end() ? (int)it->second.size() : 0;
}

// 公共传送点
std::vector<PublicWarp> const& DataStore::getPublicWarps() const { return mPublicWarps; }

bool DataStore::addPublicWarp(PublicWarp const& warp) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& w : mPublicWarps) if (w.name == warp.name) return false;
    mPublicWarps.push_back(warp);
    markDirtyLocked(F_Public);
    return true;
}

bool DataStore::removePublicWarp(std::string const& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = std::find_if(mPublicWarps.begin(), mPublicWarps.end(), [&](PublicWarp const& w) { return w.name == name; });
    if (it == mPublicWarps.end()) return false;
    mPublicWarps.erase(it);
    markDirtyLocked(F_Public);
    return true;
}

bool DataStore::updatePublicWarpPos(std::string const& name, WarpPos const& pos) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& w : mPublicWarps) {
        if (w.name == name) { w.pos = pos; markDirtyLocked(F_Public); return true; }
    }
    return false;
}

// 免申请传送点
std::vector<PrivateWarp> DataStore::getNoApprovalWarps() const {
    std::vector<PrivateWarp> result;
    for (auto& [xuid, warps] : mPrivateWarps) {
        for (auto& w : warps) {
            if (w.noApproval) result.push_back(w);
        }
    }
    return result;
}

int DataStore::countNoApprovalWarps() const {
    int n = 0;
    for (auto& [xuid, warps] : mPrivateWarps) {
        for (auto& w : warps) if (w.noApproval) n++;
    }
    return n;
}

// btp 传送点
std::vector<BlockTpPoint> const& DataStore::getBlockTpPoints() const { return mBlockTpPoints; }

bool DataStore::addBlockTpPoint(BlockTpPoint const& point) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& b : mBlockTpPoints) if (b.name == point.name) return false;
    mBlockTpPoints.push_back(point);
    markDirtyLocked(F_BlockTp);
    return true;
}

bool DataStore::removeBlockTpPoint(std::string const& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = std::find_if(mBlockTpPoints.begin(), mBlockTpPoints.end(), [&](BlockTpPoint const& b) { return b.name == name; });
    if (it == mBlockTpPoints.end()) return false;
    mBlockTpPoints.erase(it);
    markDirtyLocked(F_BlockTp);
    return true;
}

bool DataStore::updateBlockTpPoint(BlockTpPoint const& point) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& b : mBlockTpPoints) {
        if (b.name == point.name) { b = point; markDirtyLocked(F_BlockTp); return true; }
    }
    return false;
}

bool DataStore::mutateBlockTpPoint(std::string const& name, std::function<void(BlockTpPoint&)> const& fn) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& b : mBlockTpPoints) {
        if (b.name == name) { fn(b); markDirtyLocked(F_BlockTp); return true; }
    }
    return false;
}

// TPA 缓存
void DataStore::addTpaRequester(std::string const& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (std::find(mTpaRequesters.begin(), mTpaRequesters.end(), name) == mTpaRequesters.end()) {
        mTpaRequesters.push_back(name);
        markDirtyLocked(F_TpaCache);
    }
}

void DataStore::removeTpaRequester(std::string const& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto n = mTpaRequesters.size();
    mTpaRequesters.erase(std::remove(mTpaRequesters.begin(), mTpaRequesters.end(), name), mTpaRequesters.end());
    if (mTpaRequesters.size() != n) markDirtyLocked(F_TpaCache);
}

bool DataStore::hasTpaRequester(std::string const& name) const {
    return std::find(mTpaRequesters.begin(), mTpaRequesters.end(), name) != mTpaRequesters.end();
}

// 传送点申请
void DataStore::addWarpRequest(WarpRequest const& req) {
    std::lock_guard<std::mutex> lock(mMutex);
    mWarpRequests.push_back(req);
    markDirtyLocked(F_WarpReq);
}

void DataStore::removeWarpRequest(std::string const& requester, std::string const& targetPlayer) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto n = mWarpRequests.size();
    mWarpRequests.erase(std::remove_if(mWarpRequests.begin(), mWarpRequests.end(),
        [&](WarpRequest const& r) { return r.requester == requester && r.targetPlayer == targetPlayer; }),
        mWarpRequests.end());
    if (mWarpRequests.size() != n) markDirtyLocked(F_WarpReq);
}

std::vector<WarpRequest> DataStore::getWarpRequestsFor(std::string const& targetPlayer) const {
    std::vector<WarpRequest> result;
    for (auto& r : mWarpRequests) {
        if (r.targetPlayer == targetPlayer) result.push_back(r);
    }
    return result;
}

// 个人规则
std::vector<std::string> DataStore::getBlockedPlayers(std::string const& key) const {
    auto it = mPersonalRules.find(key);
    return it != mPersonalRules.end() ? it->second.blockedPlayers : std::vector<std::string>{};
}

void DataStore::setBlockedPlayers(std::string const& key, std::vector<std::string> const& list) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPersonalRules[key].blockedPlayers = list;
    markDirtyLocked(F_Rules);
}

PersonalRules DataStore::getPersonalRules(std::string const& xuid) const {
    auto it = mPersonalRules.find(xuid);
    if (it != mPersonalRules.end()) return it->second;
    return PersonalRules{};
}

void DataStore::setPersonalRules(std::string const& xuid, PersonalRules const& rules) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPersonalRules[xuid] = rules;
    markDirtyLocked(F_Rules);
}

std::vector<CrossServerEntry> const& DataStore::getCrossServers() const { return mCrossServers; }

bool DataStore::addCrossServer(CrossServerEntry const& e) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& c : mCrossServers) {
        if (c.name == e.name) return false;
    }
    mCrossServers.push_back(e);
    markDirtyLocked(F_Cross);
    return true;
}

bool DataStore::removeCrossServer(std::string const& name) {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = std::find_if(mCrossServers.begin(), mCrossServers.end(),
                           [&](CrossServerEntry const& c) { return c.name == name; });
    if (it == mCrossServers.end()) return false;
    mCrossServers.erase(it);
    markDirtyLocked(F_Cross);
    return true;
}

bool DataStore::updateCrossServer(CrossServerEntry const& e) {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& c : mCrossServers) {
        if (c.name == e.name) {
            c = e;
            markDirtyLocked(F_Cross);
            return true;
        }
    }
    return false;
}

std::unordered_map<std::string, std::vector<PrivateWarp>> const& DataStore::getAllPrivateWarps() const {
    return mPrivateWarps;
}

} // namespace mtps
