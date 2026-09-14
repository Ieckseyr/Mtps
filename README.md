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

## 随机传送怎么找安全落点

按代价从低到高依次尝试：

1. 区块已经加载在内存里，直接扫方块（数据最新）
2. 查后台预先算好的落点表（一次内存二分，零 IO）
3. 直读存档 `.ldb`（已生成但没加载的区块）
4. 都没有就用 `/tickingarea` 让引擎生成，生成完再扫

落点所在的区块不安全（大洋、悬崖）就按区块粒度扩圈找，最多 24 圈；还不行的换点重来。

## 设计说明

代码注释只留结论，来龙去脉记在这里。

### 随机传送：数据源的三次演进

| 时间 | 做法 | 为什么改 |
| --- | --- | --- |
| 09-09 初版 | `getOrLoadChunk` 直推生成 | 它只把区块入队成 Unloaded 壳对象，生成流水线不认领无视野引用的区块，状态永远停在 Unloaded → `/tpr` 卡死。`tickingarea` 才是引擎唯一暴露的"无玩家强制加载"原生机制 |
| 09-09 二次 | 加存档直读（`.ldb`） | 生成实测 32 tick 就绪，但等生成仍是秒级；已探索区域在存档里就有完整数据，毫秒级可判且不动用区块加载 |
| 09-12 三次 | 加落点预计算表 | 一次扩圈要判 ~2400 个 chunk，全落在主线程（现读 block + zstd 解压 + 物化索引 + 256 列比较）→ 吞吐上限。而索引构建阶段本来就把每个 data block 解压过一遍、只是把结果丢了；改为后台一次算完（16B/chunk），主线程查询退化成内存二分 |

就绪阈值必须 ≥ `Loaded`：生成流水线在 `CheckingForReplacementData` 之前，主区块里的子区块还是占位空数据（读出来全空气，会误判"虚空"浪费重随名额）。

存档的时效边界：`.ldb` 只反映已落盘数据，运行中新生成/改动未保存的查不到 —— 正好由内存层覆盖（刚生成/加载的必在内存），存档层只兜"已生成未加载"。

### 传送后"一片灰"的根因

玩家进服后引擎的出生状态机（`Player::SpawnPositionState`）还要跑若干 tick：
`WaitForClientAck → DetermineDimension → ChangeDimension → WaitForDimension → ChooseSpawnArea → CheckLoadedChunk → ChooseSpawnPosition → SpawnComplete`。

在这之前把玩家传送走，引擎随后仍会执行到 `ChooseSpawnPosition/SpawnComplete`，把玩家放回出生点，并把客户端的区块发布区域（`NetworkChunkPublisher`）挂在出生点。于是服务端日志"传送成功"、坐标也确实改过，但客户端一直在等出生点附近的区块 —— 站在落点什么都收不到，持续一片灰且不自愈。实测：玩家 connected 后 0.24 秒执行 `/tpr`，传送发生在 `Player Spawned` 之前 0.55 秒。

所以由 `TpUtil` 统一拦截：出生未完成不传送。异步动作（RTP）等出生完成再继续，瞬发动作（传送点 / TPA / 召集 / NPC）提示稍后重试。

常加载区域同理不能传送成功后立刻撤：靠它才加载的地块，同 tick 撤掉客户端就收不到区块数据（留 `RTP_AREA_GRACE_TICKS` 宽限期）。

### `.ldb` 的 subchunk key 格式

索引只存剥离新版后缀的标准 key，查询按前缀匹配取后缀 u64 最大者（= 最新写入）。

| 形式 | 长度 | 结构 |
| --- | --- | --- |
| 老格式主世界 | 10B | `cx(4) + cz(4) + 0x2F + subY(1)` |
| 老格式带维度 | 14B | `cx(4) + cz(4) + dim(4) + 0x2F + subY(1)` |
| 新格式 | +8B | 以上两种各加 8 字节尾部后缀（`01` + LE u64 写入序号） |

不用 `leveldb::DB::Open`：BDS 运行中占着 LOCK；Mojang 修改版的 MANIFEST/WAL 是 v2 记录格式，标准库解析失败；block 压缩类型 zlib(2)/raw-deflate(4)/zstd(5) 标准库不认识。文件以 `FILE_SHARE_READ|WRITE|DELETE` 打开，不阻塞 BDS compaction。

### 事件 ID 补丁（LeviLamina 26.40）

26.40 把事件类挪进了 inline namespace，而 EventId 基于类型名反射：Clang（官方 DLL）省略 inline ns、MSVC（本插件）保留 → 两侧字符串 hash 不一致 → MSVC 插件收不到任何 `ll::event` 事件（表现为 RTP 无响应、TPS 恒 0）。修法是在 `Mtps.cpp` 里显式特化 `getEventId` 对齐 Clang 侧的名字。

### 落盘策略

原来每次改动都全量重写全部数据文件（切一次开关 = 6 个文件 `dump(2)` + 写盘，全在主线程）。现在修改只置脏位，`tick()` 到期（默认 3 秒）合并落盘、每文件最多一次；`saveAll()` 仍立即落盘；写文件走 temp + rename 原子替换，写一半崩溃不会毁掉原数据。

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
