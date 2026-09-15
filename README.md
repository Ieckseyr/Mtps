# Mtps

Minecraft 基岩版（LeviLamina 26.40）的传送系统插件，C++ 实现。

## 功能

玩家侧

- 私人传送点：保存当前位置，点击直达，支持"免申请"共享给别人
- 公共传送点：管理员维护，所有人可传送
- 免申请传送点：别人无需申请即可传送到你的点
- 玩家互传（TPA）：发起请求、对方 /y 同意 /n 拒绝，支持弹窗或暂存两种方式
- 随机传送：预设 + 自定义半径，避开危险方块（岩浆、深水等）
- 召集：发起后其他玩家可一键聚过来
- 个人设置：拒绝所有 TPA、关闭弹窗、拒绝传送点申请
- 模糊搜索：按玩家名、传送点名、拼音/首字母查找；普通玩家只能搜到公开共享的点

管理侧

- 全部系统参数可在菜单里直接改：传送点上限、传送参数、经济系统与各项费用、默认规则
- 公共传送点增删改；填入坐标修改（输入框默认当前坐标）或一键用当前位置
- NPC 传送点：载体可选假玩家 NPC 或虚假实体，实体类型可填，支持右键/左键触发传送
- 搜索任意玩家的传送点（含私人点）并直接修改或删除

## 命令

| 命令 | 说明 |
| --- | --- |
| `/mtps` | 主菜单 |
| `/warp` | 传送点系统 |
| `/pw [编号]` | 我的传送点（编号直达） |
| `/pub [编号]` `/nap [编号]` | 公共 / 免申请传送点（编号直达） |
| `/list` | 玩家传送点浏览与搜索 |
| `/tpa` `/y` `/n` `/req` | 玩家互传、同意/加入召集、拒绝、请求管理 |
| `/tpr [编号]` | 随机传送（编号直达预设） |
| `/call` | 发起召集（再执行一次 = 取消） |
| `/set` | 个人设置 |
| `/blk` | 黑名单管理 |
| `/cs` | 跨服传送 |
| `/adm` | 系统管理（OP） |
| `/btp` | NPC 传送点管理（OP） |
| `/rld` | 重载配置与数据（OP，控制台可用） |

名字都能在 `Config.json` 的 `commands` 段里改（留空 = 不注册该指令），详见「配置文件」。

## 配置文件

`Meowdata/Mtps/Config.json`。改动可以直接编辑文件后 `/mtpsreload`，也可以在管理菜单里点。

占位符注册可以在同一个文件里配：

```json
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
```

把某一条设成空串就是关掉它，`papi.enabled=false` 则全部不注册。

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
| `WarpRequests.json` | 传送点申请 |
| `TpaCachePool.json` | TPA 请求缓存 |
| `PersonalRuleSettings.json` | 个人设置与规则 |
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
  Mtps.cpp            命令注册、事件监听、tick 驱动
  Menu.cpp            玩家侧菜单
  AdminMenu.cpp       管理员参数表单
  RandomTeleport.cpp  随机传送状态机
  LandingScan.cpp     落点预计算
  BedrockLevelReader.cpp / ArchiveScanner.*   存档直读
  NpcTeleport.cpp     假玩家 NPC / 虚假实体传送点
  WarpManager.cpp / TpaRally.cpp / Economy.cpp / DataStore.cpp / Config.cpp
  MhrAbi.*            与 MHR 的 C ABI 对接
  MtpsPapi.*          与 MeowPAPI 的 C ABI 对接
  Pinyin.* / TpUtil.* / MenuCommon.h
```
