#include "NpcTeleport.h"
#include "DataStore.h"
#include "Config.h"
#include "Menu.h"
#include "NpcSkin.h"
#include "RandomTeleport.h"
#include "Economy.h"
#include "TpUtil.h"

#include <hologramlib/HologramLib.h>

#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

#include <cmath>
#include <ctime>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MhrAbi.h"

#include <ll/api/service/Bedrock.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/item/ItemStack.h>
#include <mc/world/level/Level.h>


namespace mtps {

namespace {
// 载体创建/失败时打日志（两种载体共用）
ll::io::Logger& npcLogger() { return ll::mod::NativeMod::current()->getLogger(); }

// 管理员编辑工具: 手持"编辑物品"（可要求同时蹲下）或蹲下右键 NPC/虚假实体 → 直接打开该点管理表单。
// 默认 requireAdmin = true, 所以普通玩家不受影响, 不会挡住正常传送。
bool editToolActive(Player& player) {
    auto& cfg = Config::getInstance();
    if (!cfg.editToolEnabled()) return false;
    if (cfg.editToolRequireAdmin() && (int)player.getCommandPermissionLevel() < 1) return false;

    bool const sneaking = player.isSneaking();
    if (cfg.editToolSneak() && sneaking) return true;   // 蹲下右键

    std::string const item = cfg.editToolItem();
    if (item.empty()) return false;                     // 空串 = 不启用物品方式
    if (cfg.editToolItemRequiresSneak() && !sneaking) return false;

    auto strip = [](std::string s) {                    // 两侧都剥掉 minecraft: 前缀再比
        if (auto p = s.find(':'); p != std::string::npos) s.erase(0, p + 1);
        return s;
    };
    std::string const held = player.getSelectedItem().getTypeName();
    return !held.empty() && strip(held) == strip(item);
}
} // namespace

NpcTeleport& NpcTeleport::getInstance() {
    static NpcTeleport instance;
    return instance;
}

// 载体适配层: 两种载体在库里的差异（管理器、签名、名字牌字段名）只准出现在这里;
// 本文件其余部分只认「载体 id + isEntity」。
struct CarrierApi {
    bool isEntity{false};

    hologramlib::IPlayerNpc&    npcs() const { return hologramlib::IHologramLib::getInstance().playerNpcs(); }
    hologramlib::ICustomEntity& ents() const { return hologramlib::IHologramLib::getInstance().customEntities(); }

    bool exists(int64_t id) const { return isEntity ? ents().exists(id) : npcs().exists(id); }
    std::vector<int64_t> allIds() const { return isEntity ? ents().getAllIds() : npcs().getAllIds(); }
    bool tag(int64_t id, std::string& out) const {
        if (isEntity) {
            hologramlib::CustomEntityConfig c;
            if (!ents().get(id, c)) return false;
            out = c.nametag;
        } else {
            hologramlib::PlayerNpcConfig c;
            if (!npcs().get(id, c)) return false;
            out = c.name;
        }
        return true;
    }
    // 载体当前坐标（认领时比位置用; 两种载体的配置里都有坐标）
    bool pos(int64_t id, float& x, float& z, int& dim) const {
        if (isEntity) {
            hologramlib::CustomEntityConfig c;
            if (!ents().get(id, c)) return false;
            x = c.x, z = c.z, dim = c.dimension;
        } else {
            hologramlib::PlayerNpcConfig c;
            if (!npcs().get(id, c)) return false;
            x = c.x, z = c.z, dim = c.dimension;
        }
        return true;
    }
    void setClientYaw(int64_t id, std::string const& playerName, float yaw) const {
        if (isEntity) ents().setPlayerRotation(id, playerName, yaw, 0.0f);   // 实体接口带 pitch
        else          npcs().setPlayerRotation(id, playerName, yaw);          // NPC 接口没有 pitch
    }
    void clearClientYaws(int64_t id) const {
        if (isEntity) ents().clearPlayerRotations(id);
        else          npcs().clearPlayerRotations(id);
    }
    void destroy(int64_t id) const {
        if (isEntity) ents().destroy(id);
        else          npcs().destroy(id);
    }
};
CarrierApi carrierApi(bool isEntity) { return CarrierApi{isEntity}; }

// 载体/悬浮字的实际世界坐标
struct CarrierPos {
    float x{}, y{}, z{};
};

// 落点 = 触发点 + 世界坐标微调 (+ 该载体的高度微调); 两种载体同一套算法
CarrierPos carrierPosOf(BlockTpPoint const& p) {
    float const yOff = (p.carrier == 1) ? p.entityHeightOffset : p.npcHeightOffset;
    return CarrierPos{
        (float)p.trigger.x + p.posOffsetX,
        (float)p.trigger.y + yOff,
        (float)p.trigger.z + p.posOffsetZ,
    };
}

// 创建载体时给它的名字牌; 也是 id 失配时把它认回来的依据（两种载体同一个语义）
std::string carrierTagOf(BlockTpPoint const& p) {
    return p.displayName.empty() ? p.name : p.displayName;
}

// 唯一创建入口
void NpcTeleport::createCarrierForPoint(BlockTpPoint const& point) {
    // 重建前先收掉旧悬浮字: 悬浮字每次都是新 id, 旧的不清会在客户端叠出好几行
    destroyHologramOf(point.name);

    NpcRuntime rt;
    rt.pointName = point.name;
    rt.isEntity  = (point.carrier == 1);
    if (rt.isEntity) createEntityCarrier(point, rt);
    else             createNpcCarrier(point, rt);
    if (rt.carrierId <= 0) return;   // 失败原因各自创建函数里已打日志

    registerRuntime(point, rt);
}

// 收尾登记（两种载体共用）: 悬浮字 + runtime 表 + 点击路由表 + 日志
void NpcTeleport::registerRuntime(BlockTpPoint const& point, NpcRuntime rt) {
    // 换过 id 就把旧映射摘掉: 旧 id 已被库回收, 留着只会让一个死 id 继续路由到这个点
    auto old = mNpcs.find(point.name);
    if (old != mNpcs.end() && old->second.carrierId > 0 && old->second.carrierId != rt.carrierId) {
        mIdToPoint.erase(old->second.carrierId);
    }
    createHologramForPoint(point, rt);
    mNpcs[point.name]        = rt;
    mIdToPoint[rt.carrierId] = point.name;
    npcLogger().info("[载体] 已创建: 点={} 载体={} id={} MHR托管={}",
                     point.name,
                     rt.isEntity ? (point.entityType.empty() ? std::string("minecraft:armor_stand") : point.entityType)
                                 : std::string("假玩家NPC"),
                     rt.carrierId,
                     rt.mhrOwned ? "是" : "否");
}

// 假玩家 NPC 载体
void NpcTeleport::createNpcCarrier(BlockTpPoint const& point, NpcRuntime& rt) {
    // 皮肤: 库里不预置任何皮肤, skinId 未注册时 create() 会返回 -3（NPC 根本创建不出来）。
    // 统一过一遍解析: 点位没配皮肤 → 默认皮肤（史蒂夫）; 配了但目录里没有 → 也回落默认皮肤。
    std::string const skinId = skins::resolve(point.skinId);
    if (skinId.empty()) return;   // resolve 里已打错误日志

    CarrierPos const cp = carrierPosOf(point);
    hologramlib::PlayerNpcConfig cfg;
    cfg.name      = carrierTagOf(point);
    cfg.skinId    = skinId;
    cfg.x         = cp.x;
    cfg.y         = cp.y;   // 库把 y 原样下发, 所以"脚站在哪"= 这里给的值
    cfg.z         = cp.z;
    cfg.dimension = point.trigger.dimid;
    cfg.yaw       = point.yaw;
    cfg.scale     = point.scale;
    cfg.enabled   = point.enabled;

    int64_t const id = carrierApi(false).npcs().create(cfg);
    if (id < 0) {
        npcLogger().warn("[载体] 假玩家NPC 创建失败: 点={} 名称={} 皮肤={} 返回={}（-3=皮肤未注册）",
                         point.name, cfg.name, cfg.skinId, id);
        return;
    }
    rt.carrierId = id;
}

// 虚假实体载体: AddActorPacket 直发客户端, 不占服务端实体系统; 实体类型可填
void NpcTeleport::createEntityCarrier(BlockTpPoint const& point, NpcRuntime& rt) {
    auto& entMgr = hologramlib::IHologramLib::getInstance().customEntities();

    CarrierPos const cp = carrierPosOf(point);
    hologramlib::CustomEntityConfig cfg;
    cfg.identifier        = point.entityType.empty() ? "minecraft:armor_stand" : point.entityType;
    cfg.x                 = cp.x;
    cfg.y                 = cp.y;
    cfg.z                 = cp.z;
    cfg.dimension         = point.trigger.dimid;
    cfg.yaw               = point.yaw;
    cfg.nametag           = carrierTagOf(point);
    cfg.nametagAlwaysShow = true;
    cfg.scale             = point.scale;
    cfg.enabled           = point.enabled;
    cfg.viewDistance      = 96.0;

    // 优先交给 MHR 托管（ownerKey 幂等: 重启后不会两边各建一个）。
    // 按 ABI 约定对方会再减 2 格（锚点补偿）, 所以这里补 +2 —— 补偿只在此处出现一次。
    constexpr float   kMhrYCompensation = 2.0f;
    std::string const ownerKey          = "mtps:" + point.name;
    int64_t           mhrUid = -1;
    int64_t           libId  = -1;
    if (mhrabi::available()) {
        mhrUid = mhrabi::spawn(ownerKey, cfg.identifier, cfg.nametag, cfg.x,
                               cp.y + kMhrYCompensation, cfg.z, cfg.dimension, cfg.yaw, cfg.scale, &libId);
        if (mhrUid > 0 && libId > 0) {
            npcLogger().info("[载体] 虚假实体已交给 MHR 托管: 点={} MHRuid={} 库内id={} 高度={}",
                             point.name, mhrUid, libId, cp.y);
        } else {
            npcLogger().warn("[载体] MHR 托管未成功, 回退自建: 点={}", point.name);
            mhrUid = -1;
            libId  = -1;
        }
    }

    // ② 回退自建（MHR 未安装 / 未提供该 ABI）: 库不做任何 y 补偿（mPosition 原样下发）
    if (libId <= 0) {
        int64_t const stableId = 100000 + (int64_t)(std::hash<std::string>{}(point.name) & 0x7FFFFFFF) % 900000;
        libId                  = entMgr.createWithId(cfg, stableId);
        if (libId < 0) libId   = entMgr.createRandom(cfg);
        if (libId < 0) {
            npcLogger().warn("[载体] 虚假实体创建失败: 点={} 类型={}（实体标识符是否正确?）",
                             point.name, cfg.identifier);
            return;
        }
    }
    rt.carrierId = libId;
    rt.mhrUid    = (mhrUid > 0) ? mhrUid : -1;
    rt.mhrOwned  = (mhrUid > 0);
}

// 常显悬浮字: 高度相对**载体**算（载体被微调抬高时字跟着走）;
// 文本里的换行拆成多行下发（表单约定用 "\n" 字面量输入, 存进数据时已解码成真换行）
void NpcTeleport::createHologramForPoint(BlockTpPoint const& point, NpcRuntime& rt) {
    if (!point.floatingTextEnabled || point.floatingText.empty()) return;
    auto&            textMgr = hologramlib::IHologramLib::getInstance().holograms();
    CarrierPos const cp      = carrierPosOf(point);
    int64_t          holoId  = textMgr.create(cp.x, cp.y + point.floatingOffsetY, cp.z);
    if (holoId <= 0) return;
    std::size_t start = 0;
    while (true) {
        auto const nl = point.floatingText.find('\n', start);
        textMgr.addLine(holoId, point.floatingText.substr(start, nl == std::string::npos ? nl : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    textMgr.setColor(holoId, 1.0f, 1.0f, 1.0f, 1.0f);
    textMgr.draw(holoId);
    rt.holoId = holoId;
}

// 收掉某点位现有的悬浮字（重建载体前必须调用: 悬浮字每次都是新 id,
// 旧的不 destroy 会留在客户端, 改几次高度就叠出几行）
void NpcTeleport::destroyHologramOf(std::string const& pointName) {
    auto it = mNpcs.find(pointName);
    if (it == mNpcs.end() || it->second.holoId <= 0) return;
    hologramlib::IHologramLib::getInstance().holograms().destroy(it->second.holoId);
    it->second.holoId = -1;
}

void NpcTeleport::destroyCarrier(std::string const& pointName) {
    auto it = mNpcs.find(pointName);
    if (it == mNpcs.end()) return;
    auto& rt = it->second;

    if (rt.carrierId > 0) {
        if (rt.isEntity && rt.mhrOwned) {
            mhrabi::despawn("mtps:" + pointName);   // MHR 拥有 → 请它移除（含持久化记录）
        } else {
            carrierApi(rt.isEntity).destroy(rt.carrierId);
        }
        mIdToPoint.erase(rt.carrierId);
    }
    if (rt.holoId > 0) {
        hologramlib::IHologramLib::getInstance().holograms().destroy(rt.holoId);
    }
    mNpcs.erase(it);
}

void NpcTeleport::destroyAllNpcs() {
    auto& holo = hologramlib::IHologramLib::getInstance();
    for (auto& [name, rt] : mNpcs) {
        if (rt.carrierId > 0 && !(rt.isEntity && rt.mhrOwned)) carrierApi(rt.isEntity).destroy(rt.carrierId);
        if (rt.holoId > 0) holo.holograms().destroy(rt.holoId);
    }
    mNpcs.clear();
    mIdToPoint.clear();
}

void NpcTeleport::init() {
    destroyAllNpcs();
    for (auto& point : DataStore::getInstance().getBlockTpPoints()) {
        createCarrierForPoint(point);
    }
}

void NpcTeleport::reloadAll() {
    init();
}

// 只重建一个传送点的载体与悬浮字（数据层已是最新）
void NpcTeleport::refreshPoint(std::string const& pointName) {
    auto it                = mNpcs.find(pointName);
    bool const wasMhrOwned = (it != mNpcs.end() && it->second.isEntity && it->second.mhrOwned);

    BlockTpPoint point;
    bool found = false;
    for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
        if (p.name == pointName) { point = p; found = true; break; }
    }
    if (!found) {              // 点位已删除: 只移除载体
        destroyCarrier(pointName);
        return;
    }
    // 托管给 MHR 的实体只原地更新（同 ownerKey 幂等）: 销毁会把 MHR 的记录一起删掉, 还得换个 uid
    bool const sameKind = (it != mNpcs.end() && it->second.isEntity == (point.carrier == 1));
    if (wasMhrOwned && sameKind && point.enabled) {
        createCarrierForPoint(point);
        return;
    }
    destroyCarrier(pointName);
    if (point.enabled) createCarrierForPoint(point);
}

// 朝向数学（与 MHR 一致）: yawToward 用 atan2(-dx, dz), 逐客户端下发
namespace {
float normalizeYaw(float y) {
    while (y > 180.0f) y -= 360.0f;
    while (y < -180.0f) y += 360.0f;
    return y;
}
float yawToward(float fromX, float fromZ, float toX, float toZ) {
    float const dx = toX - fromX;
    float const dz = toZ - fromZ;
    if (std::fabs(dx) < 1e-4f && std::fabs(dz) < 1e-4f) return 0.0f;   // 水平重合保持朝南
    return normalizeYaw(std::atan2(-dx, dz) * (180.0f / 3.14159265358979323846f));
}
} // namespace

// 载体 id 的重新认领: 库内 id 不保证稳定（经 MHR 改过属性后会变）, 而点击路由与
// 逐客户端朝向都按 id 记账 —— 按"创建时给它的名字牌"把它认回来, 两种载体同一套。

int64_t NpcTeleport::currentCarrierIdOf(BlockTpPoint const& point) const {
    CarrierApi const  api = carrierApi(point.carrier == 1);
    std::string const tag = carrierTagOf(point);

    int64_t byTag = -1;
    double  byTagD2 = 0.0;
    int64_t byPos = -1;
    double  byPosD2 = 0.0;
    int     posCandidates = 0;
    for (int64_t id : api.allIds()) {
        std::string candTag;
        float       cx = 0, cz = 0;
        int         cdim = 0;
        if (!api.tag(id, candTag) || !api.pos(id, cx, cz, cdim)) continue;
        if (cdim != point.trigger.dimid) continue;
        double const dx = (double)cx - point.trigger.x;
        double const dz = (double)cz - point.trigger.z;
        double const d2 = dx * dx + dz * dz;
        if (candTag == tag && (byTag < 0 || d2 < byTagD2)) byTag = id, byTagD2 = d2;
        if (d2 <= 4.0) {          // 触发点 2 格内
            posCandidates++;
            if (byPos < 0 || d2 < byPosD2) byPos = id, byPosD2 = d2;
        }
    }
    if (byTag >= 0) return byTag;
    // 名字牌对不上（管理员改过名?）: 只有触发点旁"唯一"一个载体时才认, 多候选不猜
    return (posCandidates == 1) ? byPos : -1;
}

std::string NpcTeleport::pointNameByCarrierId(bool isEntity, int64_t id) const {
    std::string tag;
    if (!carrierApi(isEntity).tag(id, tag) || tag.empty()) return {};
    for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
        if (carrierTagOf(p) == tag) return p.name;
    }
    return {};
}

void NpcTeleport::adoptCarrierId(std::string const& pointName, NpcRuntime& rt, int64_t newId) {
    if (rt.carrierId > 0) mIdToPoint.erase(rt.carrierId);
    rt.carrierId  = newId;
    rt.staleTicks = 0;
    mIdToPoint[newId] = pointName;
    npcLogger().info("[载体] id 已重新认领（重建过）: 点={} 载体={} 新id={}",
                     pointName, rt.isEntity ? "虚假实体" : "假玩家NPC", newId);
}

// 每 tick: 载体 id 自愈 + 开启"看向玩家"的载体逐客户端看向观察者（每个玩家看到的都是"它看着我"）
void NpcTeleport::tick() {
    static uint32_t sLookTick = 0;
    if ((++sLookTick % 2) != 0) return;          // 10Hz 节流（与 MHR 同）
    if (mNpcs.empty()) return;

    auto level = ll::service::getLevel();
    if (!level) return;

    // 认不回来先按名字牌认（不改动载体, 无闪烁）; 连续 3 秒都认不回来才重建
    {
        std::vector<std::string> needRebuild;
        for (auto& [name, rt] : mNpcs) {
            if (rt.carrierId > 0 && carrierApi(rt.isEntity).exists(rt.carrierId)) {
                rt.staleTicks = 0;
                continue;
            }
            BlockTpPoint point;
            bool found = false;
            for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
                if (p.name == name) { point = p; found = true; break; }
            }
            if (!found || !point.enabled) continue;

            if (int64_t const fresh = currentCarrierIdOf(point); fresh > 0 && fresh != rt.carrierId) {
                adoptCarrierId(name, rt, fresh);
                continue;
            }
            if (++rt.staleTicks >= 30) {       // 10Hz × 30 ≈ 3 秒
                rt.staleTicks = 0;
                needRebuild.push_back(name);
            }
        }
        for (auto& name : needRebuild) {
            for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
                if (p.name == name && p.enabled) {
                    npcLogger().warn("[载体] 认不回来, 重建: 点={} 载体={}", name,
                                     p.carrier == 1 ? "虚假实体" : "假玩家NPC");
                    createCarrierForPoint(p);   // 幂等: MHR 同 ownerKey 不会多建
                    break;
                }
            }
        }
    }

    struct LookPlayer { std::string name; float x, z; int dim; };
    std::vector<LookPlayer> players;
    level->forEachPlayer([&](Player& p) -> bool {
        auto const& pos = p.getPosition();
        players.push_back({p.getRealName(), pos.x, pos.z, (int)p.getDimensionId()});
        return true;
    });
    if (players.empty()) return;

    for (auto& [pointName, rt] : mNpcs) {
        if (rt.carrierId <= 0) continue;
        BlockTpPoint point;
        bool found = false;
        for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
            if (p.name == pointName) { point = p; found = true; break; }
        }
        if (!found || !point.lookAtPlayers) continue;
        CarrierApi const api = carrierApi(rt.isEntity);
        for (auto const& pl : players) {
            if (pl.dim != point.trigger.dimid) continue;
            api.setClientYaw(rt.carrierId, pl.name,
                             yawToward((float)point.trigger.x, (float)point.trigger.z, pl.x, pl.z));
        }
    }
}

// 朝向开关变化后立即生效（关闭时清除逐客户端覆盖, 否则载体还会保持"看着每个人"）
void NpcTeleport::applyLookMode(std::string const& pointName) {
    auto it = mNpcs.find(pointName);
    if (it == mNpcs.end() || it->second.carrierId <= 0) return;

    BlockTpPoint point;
    bool found = false;
    for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
        if (p.name == pointName) { point = p; found = true; break; }
    }
    if (found && point.lookAtPlayers) return;   // 开启由 tick 驱动
    carrierApi(it->second.isEntity).clearClientYaws(it->second.carrierId);
}

// 载体交互（ghost 事件唯一入口; domain 只用于兜底认领, 路由本身只看载体 id）
void NpcTeleport::onInteract(std::string const& domain, std::string const& playerName, int64_t id, int action) {
    auto it = mIdToPoint.find(id);
    if (it != mIdToPoint.end()) {
        handleInteract(playerName, it->second, action);
        return;
    }
    // 认不出来的 id = 刚重建过（id 换了）。按名字牌反查点位并顺手补账,
    // 免得"刚好赶在自愈那一拍之前点的"点上去没反应
    std::string const name = pointNameByCarrierId(domain == "entity", id);
    if (name.empty()) return;
    auto rit = mNpcs.find(name);
    if (rit == mNpcs.end()) return;
    adoptCarrierId(name, rit->second, id);
    handleInteract(playerName, name, action);
}

// 交互公共逻辑（两种载体共用）: 玩家 + 点位名 → 校验后触发传送
void NpcTeleport::handleInteract(std::string const& playerName, std::string const& pointName, int action) {
    auto level = ll::service::getLevel();
    if (!level) return;
    Player* player = level->getPlayer(playerName);
    if (!player) return;

    BlockTpPoint point;
    bool found = false;
    for (auto& p : DataStore::getInstance().getBlockTpPoints()) {
        if (p.name == pointName) { point = p; found = true; break; }
    }
    if (!found) return;

    // 管理员编辑模式优先于传送与触发开关: 手持编辑物品 / 蹲下 + 右键 → 打开该点的管理表单。
    // 放在"是否启用"判断之前, 禁用的点也能这样打开来改。
    // 1=右键交互 4=交互更新（部分客户端蹲下右键发的是 4）, 两种都当"右键"
    if ((action == 1 || action == 4) && editToolActive(*player)) {
        player->sendMessage("§7[编辑模式] 已打开 §e" + point.name + "§7 的管理表单（松开蹲下/放下物品即可正常传送）");
        menu::openBlockTpDetail(*player, point.name);
        return;
    }
    if (!point.enabled) return;

    // 触发方式开关: 1=右键交互 / 2=左键攻击（其余动作不触发）
    if (action == 1 && !point.triggerOnInteract) return;
    if (action == 2 && !point.triggerOnAttack) return;
    if (action != 1 && action != 2) return;

    teleportPlayerToPoint(*player, point);
}

void NpcTeleport::teleportPlayerToPoint(Player& player, BlockTpPoint const& point) {
    // 该点的冷却（0 = 不限制）, 键带上玩家名所以各玩家互不影响
    if (point.cooldownSeconds > 0) {
        std::string const key  = player.getRealName() + "|" + point.name;
        int64_t const   now    = (int64_t)std::time(nullptr);
        auto            it     = mLastUse.find(key);
        if (it != mLastUse.end()) {
            int64_t const left = point.cooldownSeconds - (now - it->second);
            if (left > 0) {
                player.sendMessage("§c[传送点] §f冷却中, 还需等待 §e" + std::to_string(left) + "§f 秒");
                return;
            }
        }
        mLastUse[key] = now;
    }

    // 经济
    if (point.economy.enabled && point.economy.cost > 0 && Config::getInstance().economyEnabled()) {
        if (!Economy::getInstance().canAfford(player, point.economy.cost)) {
            player.sendMessage("§c[传送点] §f余额不足");
            return;
        }
        Economy::getInstance().withdraw(player, point.economy.cost);
    }

    if (point.isRandom) {
        // 随机传送
        RtpOptions opts;
        opts.dimid = point.randomDimid;
        opts.originX = point.randomOriginX;
        opts.originZ = point.randomOriginZ;
        opts.radius = point.randomRadius;
        // 中心模式: entity = 以触发实体(NPC/实体)自身位置为中心（用触发点坐标, 即 NPC 所在处）
        // 随机中心三选一: 世界中心 / 创建时 NPC 所在位置 / 触发玩家所在位置
        // （旧数据里的 fixed / entity 都按"创建时 NPC 位置"处理）
        std::string const mode = point.randomOriginMode;
        if (mode == "world") {
            opts.originMode = "fixed";
            opts.originX    = 0;
            opts.originZ    = 0;
        } else if (mode == "player") {
            opts.originMode = "player";   // 由 RandomTeleport 取发起者坐标
        } else {
            opts.originMode = "fixed";
            opts.originX    = point.randomOriginX;
            opts.originZ    = point.randomOriginZ;
        }
        opts.message = point.message.empty() ? "§a[随机传送] §f已传送！" : point.message;
        opts.cost = 0; // 已扣费
        RandomTeleport::getInstance().start(player, opts);
    } else {
        // 固定传送
        if (!teleportPlayerIfReady(player, Vec3((float)point.target.x, (float)point.target.y, (float)point.target.z),
                                   (::DimensionType)point.target.dimid)) {
            player.sendMessage("§c[传送] §f出生点还在加载中，请稍候再试");
            return;
        }
        if (!point.message.empty()) player.sendMessage(point.message);
    }
}

} // namespace mtps
