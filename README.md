# Mtps

Minecraft 基岩版（LeviLamina 26.40）的传送系统插件，C++ 实现。

## 功能

玩家侧

- 私人传送点：保存当前位置，点击直达，支持"免申请"共享给别人
- 公共传送点：管理员维护，所有人可传送
- 免申请传送点：别人无需申请即可传送到你的点
- 玩家互传（TPA）：发起请求，对方 `/y` 同意、`/n` 拒绝，侧边栏可用 PAPI 提示
- 请求管理：待处理请求一览，可逐条同意 / 取消；同意入口也用于响应召集（拒绝用 `/n`）
- 随机传送：只走管理员添加的预设（半径、维度、冷却、费用各自可配），落点避开危险方块
- 召集：发起后其他人可一键聚过来（再点一次 = 取消）
- 跨服传送：一键转到其它服务器（地址由管理员维护）
- 黑名单：名单里的玩家不能向你发起传送
- 个人设置：拒绝所有 TPA、关闭弹窗、拒绝传送点申请
- 模糊搜索：按玩家名、传送点名、拼音/首字母查找

管理侧

- 全部系统参数可在菜单里直接改：传送点上限、传送参数、经济系统与各项费用、默认规则
- 公共传送点增删改；填入坐标修改（输入框默认当前坐标）或一键用当前位置
- 随机传送预设增删改：名称、原点（固定 / 以玩家为原点）、半径、维度、冷却、费用
- NPC 传送点：载体可选假玩家 NPC 或虚假实体，实体类型可填，右键 / 左键触发传送
- 每个点可单独调：传送冷却、触发方式、看向玩家、悬浮字、高度与水平微调、皮肤
- 皮肤：从 `npc_skins` 目录取，没配就用内置史蒂夫；MHR 存过的皮肤也能直接用
- 编辑工具：手持指定物品或蹲下右键 NPC / 实体，直接打开该点的管理表单
- 传送类型可在菜单里互切（定点 ↔ 随机），切换时自动补齐对侧参数
- 搜索任意玩家的传送点（含私人点）并直接修改或删除

## 命令

| 命令 | 说明 |
| --- | --- |
| `/mtps` | 主菜单 |
| `/warp` | 传送点系统 |
| `/pw [编号]` | 我的传送点（编号直达） |
| `/pub [编号]` `/nap [编号]` | 公共 / 免申请传送点（编号直达） |
| `/list` | 玩家传送点浏览与搜索 |
| `/tpa` `/y` `/n` `/req` | 玩家互传、同意 / 加入召集、拒绝、请求管理 |
| `/tpr [编号]` | 随机传送（编号直达预设） |
| `/call` | 发起召集（再执行一次 = 取消） |
| `/set` | 个人设置 |
| `/blk` | 黑名单管理 |
| `/cs` | 跨服传送 |
| `/adm` | 系统管理（OP） |
| `/btp` | NPC 传送点管理（OP） |
| `/rld` | 重载配置与数据（OP，控制台可用） |

名字都能在 `Config.json` 的 `commands` 段里改：键名固定、值就是指令名，**留空 = 不注册该指令**。
名字首字符必须是字母或下划线，其余只能是字母 / 数字 / 下划线；重名或不合法的会被跳过并在启动日志里告警（超过 4 个字母也会提示改短）。

## 配置文件

`Meowdata/Mtps/Config.json`。改动可以直接编辑文件后 `/rld`，也可以在管理菜单里点。

```json
{
  "commands": { "menu": "mtps", "private": "pw", "reload": "rld" },
  "skins": {
    "extraDirs": ["plugins/MeowHolographicRenderer/config/npc_skins"]
  },
  "blockTeleport": {
    "editTool": {
      "enabled": true,
      "item": "minecraft:nether_star",
      "itemRequiresSneak": false,
      "sneak": true,
      "requireAdmin": true
    }
  },
  "randomTeleport": {
    "cooldownSeconds": 120,
    "preferKnownLandings": false,
    "presets": [
      { "name": "主世界随机", "radius": 1000, "dimid": 0, "cooldown": 0 }
    ]
  },
  "papi": {
    "enabled": true,
    "formats": {
      "mtps_tpa": "§6[TPA] §e{player} §7{type}",
      "mtps_tpa_count": "§e{count}",
      "mtps_warps": "§e{count}§7/§a{max}",
      "mtps_shared": "§e{count}",
      "mtps_public": "§e{count}"
    }
  }
}
```

上面是节选，实际文件里各段都是完整的。几处要点：

- `commands`：指令名，规则见上一节。
- `skins.extraDirs`：额外的皮肤来源目录（默认指向 MHR 的皮肤目录，认其中的 `*.bin` 快照）。
- `blockTeleport.editTool`：管理员编辑工具，`item` 填物品名（留空 = 只认蹲下）。
- `randomTeleport.cooldownSeconds`：全局随机传送冷却；预设自己的 `cooldown` 优先，
  `0` = 用全局、`>0` = 用该值。
- `randomTeleport.preferKnownLandings`：选点时优先取"存档里已知安全"的已生成区块。
  打开后传送基本是即时的（不用等地形生成），代价是落点只会落在已探索范围内；
  关（默认）时仍按圆盘均匀随机选点，适合想让玩家散到新地形的服。
- `papi`：占位符注册开关与格式（某条设为空串 = 关掉该占位符）。

## PAPI 占位符

走 MeowPAPI 的 C ABI 注册（不链接它的静态库，也就不依赖 lrca）。MeowPAPI 没装或者
版本不符时自动跳过，不影响其它功能。

| 占位符 | 内容 | 可用的替换字段 |
| --- | --- | --- |
| `mtps_tpa` | 最近一条待处理的传送请求，没人请求时为空 | `{player}` `{type}` |
| `mtps_tpa_count` | 待处理请求数量 | `{count}` |
| `mtps_warps` | 自己的私人传送点数量 | `{count}` `{max}` |
| `mtps_shared` | 自己共享出去的传送点数量 | `{count}` |
| `mtps_public` | 公共传送点总数 | `{count}` |

## 数据文件

| 文件 | 说明 |
| --- | --- |
| `PrivateWarps.json` | 私人传送点（按 xuid） |
| `PublicWarps.json` | 公共传送点 |
| `BlockTeleportPoints.json` | NPC 传送点 |
| `CrossServer.json` | 跨服目的地 |
| `WarpRequests.json` | 传送点申请 |
| `TpaCachePool.json` | TPA 请求缓存 |
| `PersonalRuleSettings.json` | 个人设置与规则 |
| `npc_skins/` | 皮肤目录（`<id>.png` 或 `<id>/` 子文件夹；`default.png` 为内置默认） |
| `pinyin.txt` | 搜索用的拼音表（可选，每行 `汉字 拼音`） |

写盘是合并式的：改动先记脏，最多 3 秒落一次盘，写文件走临时文件 + 改名，写失败下次重试。
关服时会全部落盘。

## 对外接口

Mtps 的虚假实体载体可以交给 MeowHolographicRenderer（MHR）托管，前提是对方提供这几个
导出（C ABI；Mtps 用 GetProcAddress 解析 + 版本握手，没有或主版本不符就回退为自建实体）：

```c
uint32_t MHR_GetAbiVersion(void);
int64_t  MHR_SpawnEntity(const char* ownerKey, const char* identifier, const char* nametag,
                         float x, float y, float z, int dim, float yaw, float scale, int64_t* outLibId);
bool     MHR_DespawnEntity(const char* ownerKey);
```

`ownerKey` 是幂等键（`mtps:<点位名>`），同一个键重复登记只会有一个实体。

### 假玩家 NPC 与虚假实体

两种载体的差异（管理器、接口签名、名字牌字段名）只允许出现在 `NpcTeleport.cpp` 的 `CarrierApi` 适配层，上层只认「载体 id + isEntity」。两种载体的 id 段互不相交（NPC 自增段从 1 起，虚假实体固定在 `[0x10000000, 0x7FFFFFFF)`），所以点击路由共用一张表。

库内 id 不保证稳定（例如经 MHR 改过实体属性之后会变），所以 id 只当缓存：失效时按"创建时给它的名字牌"认回来，连续 3 秒认不回来才重建。

高度微调按载体各一个（客户端对 `AddActor` 与 `AddPlayer` 的 y 原点未必一致），竖直方向不再另设共用的 Y —— 两个设成同值就是整体平移。悬浮字高度相对载体算，载体抬高时字跟着走。

### 皮肤来源

- `Meowdata/Mtps/npc_skins/`：`<id>.png`，或 `<id>/` 子文件夹（内含 PNG + 可选 `.json` 几何）
- `Config.skins.extraDirs`（默认指向 MHR 的目录）：认 `*.bin` 快照（HologramLib 的持久化格式）
- 库里已注册的（与 MHR 共用同一个 HologramLib 实例，它注册过的直接可用）

库不预置任何皮肤，`skinId` 未注册时 `create()` 直接返回 -3，所以"没配皮肤"用内置史蒂夫配色：首次启动生成 `npc_skins/default.png`，覆盖它即可换。

## 随机传送怎么生成新区块

落点判定按代价从低到高：内存里的区块（数据最新）→ 后台预先算好的落点表（内存二分）→
直读存档 `.ldb`（已生成但没加载）。三者都没有 = 这块地从没生成过，此时登记一个**临时常加载
区域**，让引擎自己把它生成出来。

临时区域走引擎 API（`TickingAreasManager::_addArea`），不是 `/tickingarea` 命令：不落盘、
重启不会被预加载、跳过区域个数上限，用完直接移除。注意 `Bounds` 的 x/z 单位是**区块**（y 是方块）。

区块就绪后才传送；落点若落在区域之外（扩圈命中的角落）会先把区域改挂到落点、等它就绪再传
—— 否则玩家会落进没有区块数据的位置（表现为一片虚空）。

几个提速上的取舍：等生成时只挂半径 2 的小区域（引擎按块处理，区域越大中心越晚轮到，
半径 4 要处理 81 块、半径 2 只 25 块），传送前再扩到半径 4 并保留宽限期。扩圈搜索里"表里没有
这个区块"也算廉价查询（一次二分），否则整片没生成过的荒野会把每 tick 的扫描额度吃光；
重随名额用尽时会退回"存档已知安全点"再试一次，而不是直接放弃。选点策略见
`preferKnownLandings`。

## 构建

需要 xmake 与 MSVC（C++23）：

```
xmake build
```

产物是 `Mtps.dll`，构建后自动拷到工程目录和 `bin/Mtps/`。

依赖：LeviLamina 26.40、HologramLib（协议实体 / 假玩家 NPC / 悬浮字）、zlib、zstd。

## 目录

```
src/
  Mtps.cpp                     命令注册、事件监听、tick 驱动
  Menu.cpp                     主菜单、随机传送菜单、搜索、黑名单、个人设置
  WarpMenu.cpp                 传送点系统、私人 / 公共 / 免申请、跨服
  BlockTpMenu.cpp              NPC 传送点管理（创建 / 编辑 / 载体 / 外观）
  TpaMenu.cpp                  玩家互传、请求管理
  AdminMenu.cpp                管理员参数表单
  RandomTeleport.cpp           随机传送状态机（会话 / 探测 / 扫描 / 扩圈）
  RandomTeleportInternal.h     状态机内部共享声明
  SurfaceScan.cpp              地表扫描与落点判定
  LandingScan.cpp              落点预计算
  TickingAreaUtil.cpp          常加载区域（引擎 API）与残留清理
  BedrockLevelReader.*         存档 .ldb 直读
  ArchiveScanner.*             存档直读的后台封装（索引 + 落点表）
  NpcTeleport.*                假玩家 NPC / 虚假实体载体
  NpcSkin.* / NpcSkinSteve.h   皮肤来源管理与内置默认皮肤
  WarpManager.cpp / TpaRally.cpp / Economy.cpp
  DataStore.* / Config.* / DataTypes.h
  MhrAbi.*                     与 MHR 的 C ABI 对接
  MtpsPapi.*                   与 MeowPAPI 的 C ABI 对接
  Pinyin.* / TpUtil.* / MenuCommon.h / MemoryOperators.cpp
```
