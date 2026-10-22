# 多端同步设计（数据格式 v2）

> 状态：**M1 已实施**（共享层 + macOS 宿主 1.11.0；Windows/Linux 宿主设置段随 #7/#8 落地）。
> 进度追踪 [#9](https://github.com/turinglambdaai/payback/issues/9)。存储布局的权威描述
> 在 [`data-format.md`](data-format.md)；本文件保留「为什么」与不变式——改 `app/sync.rkt`、
> 做任何同步相关 UI 之前必读。
>
> 实施注记（对方案的唯一偏离）：M1 的目录监听用**宿主 5 秒轮询 `sync-state` + 引擎侧
> stat 指纹**实现，而不是 FSEvents/ReadDirectoryChangesW/inotify 原生监听——网盘挂载点
> 的变更通知本就不可靠（方案自留 30 秒轮询兜底正是为此），stat 轮询平台中立、挂载安全、
> 毫秒级成本。原生事件监听留作后续优化，轮询语义不变。

## 目标与不变式

1. **本地优先、永不建服务器。** 多端同步走用户自己的同步方案（网盘文件夹 / iCloud /
   后续 WebDAV·S3 适配器）。我们不运营任何同步服务、不碰用户数据。这是商业模式
   （买断制、服务器成本恒为零）的前提，不是可商量的实现细节。
2. **数据只录一次。** 任一端录入的设备记录，其余端自动收敛到同一视图。
3. **合并语义在数据格式里，不在同步引擎里。** 必须在任何哑文件同步引擎下保持正确——
   包括会生成冲突副本的（Dropbox / OneDrive）和会静默「最后写入胜出」的（iCloud Drive）。
4. **任意两台机器看到同一目录状态，合并出同一视图。** 合并是确定性的、与文件扫描顺序
   无关、幂等的。

## 已否决的路线（不要重新引入）

- **单文件 `payback.json` + 整文档 LWW + 冲突提示**（roadmap v1.4 原表述）：两台机器写
  同一个文件，同步引擎要么产冲突副本、要么静默覆盖，必丢数据。
- **同步数据库 / 二进制文件**（SQLite 等）：同步引擎并发写必损坏。payback 没有这种形态；
  其他 rivet 产品借鉴本方案时同样禁止。
- **共享 manifest / 索引文件**：任何多写者文件都是冲突面。文件列表本身就是清单。

## 存储布局 v2

数据目录 = 本机默认目录（平台路径见 `data-format.md`），或用户在设置里指定的同步文件夹。

```
devices-<machineId>.json   # 每台机器一个分片，只含本机创建/编辑过的记录
payback.json               # v1 遗留：只读、一次性迁移后不再写
```

### 分片 schema

```json
{
  "version": 2,
  "machine": { "id": "m-<8hex>", "name": "MacBook Pro", "updatedAt": "yyyy-MM-dd HH:mm:ss" },
  "devices": [
    {
      "id": "d-...", "name": "...", "icon": "...", "category": "...",
      "priceMinor": 0, "currency": "CNY", "purchaseDate": "yyyy-MM-dd",
      "willingPerDayMinor": null, "notes": "",
      "createdAt": "...", "updatedAt": "...",
      "editedBy": "m-...", "deletedAt": null
    }
  ],
  "settings": {
    "currency":   { "value": "CNY", "updatedAt": "...", "editedBy": "m-..." },
    "licenseKey": { "value": "PB1....", "updatedAt": "...", "editedBy": "m-..." }
  }
}
```

设备记录的存储字段与校验规则沿用 `data-format.md`，v2 只新增 `editedBy` 与 `deletedAt`。

### 规则

- **machineId**：每个安装一次性随机生成（`m-` + 8 hex），存本机、不进同步内容；分片文件名
  与 `machine.id` 各带一份，以文件名为准。`machine.name` 是用户可改的显示名，只写进自己
  的分片。
- **写入面 = claim 集**：本机只写 `devices-<自己 machineId>.json`，内容只含本机创建或编辑
  过的记录。**不镜像他机记录**——否则旧副本会被当成本机的新主张反复回声。他机创建的
  设备在本机被编辑或删除时，本机把该记录（或其 tombstone）写进自己的分片，即完成认领。
- **删除 = tombstone**：置 `deletedAt`、记录保留。真删在 union 合并下会让设备从他机
  「复活」。tombstone 永不回收（账本级数据量不需要 GC）。
- **记录级 LWW**：同 id 记录按 `(updatedAt, editedBy)` 字典序取最大者。确定性来自第二键——
  时间相同时 machineId 决胜。时钟偏差可能让「旧」的赢，但所有机器收敛到同一结果，用户
  重编辑即纠正；低频写的账本场景可接受。
- **settings 同步白名单 = `currency`、`licenseKey`**，逐 key 同样 LWW。其余 settings
  （`updateAutoCheck` / `updateBaseUrl` / `lastUpdateCheckAt` / `rolloutBucket` /
  `lastDigestAt` / `seenMilestones`）**保持本机不同步**——里程碑庆祝是每台设备各自的
  第一手体验，更新检查是本机状态。
- **id 碰撞**：`d-<10hex>` 40 bit，账本级数据量下概率可忽略；同 id 即同设备，走 LWW。

## 合并算法

读时合并，**永不回写他机分片**：

1. 扫描目录全部 `devices-*.json`。忽略 dotfile、`*.tmp`、解析失败或 shape 不对的文件
   （网盘上传会产生半文件）——跳过并在 `sync-state` 上报，绝不删、绝不炸。
2. 设备按 `id` union；逐 id 取 `(updatedAt, editedBy)` 最大者；胜者带 `deletedAt` 即从
   视图隐藏。
3. settings 逐 key 同规则。
4. computed / summary 从合并视图派生，不落盘（同 v1 纪律）。

本机的内存视图 = 合并结果；本机写操作只改自己的 claim 集并重写自己的分片，再重合并。

## 触发与监听

- 启动合并一次（几个小 JSON，毫秒级）。
- 桌面端监听数据目录：FSEvents（macOS）/ ReadDirectoryChangesW（Windows）/ inotify
  （Linux），**30 秒轮询兜底**——网盘挂载点的变更通知不可靠。
- 外部变更防抖 1–2 秒后重合并，结果经 `load-all` 让宿主刷新。
- 本地写沿用现有原子替换 + semaphore 纪律；只写自己的分片。

## 平台落点与 UX

| 平台 | 同步位置 | 用户动作 |
|---|---|---|
| macOS / Windows / Linux | 任意同步文件夹（iCloud Drive / Dropbox / OneDrive / 坚果云 / Syncthing / Resilio） | 每台一次：自动探测常见位置并提示「检测到 X，使用？」，探测不到才开 folder picker；默认仍本机不同步 |
| iOS / iPadOS（v2.0） | iCloud Drive 容器（Apple 运营，零服务器） | 纯开关，路径固定无需填 |
| watchOS（v2.0） | 不参与文件同步 | iPhone 的只读伴随端，WatchConnectivity 推合并视图 |
| Android（v2.x） | WebDAV / S3 适配器 | 每端填一次地址 + 凭据 |

「数据只录一次」之外的第二底线：**同步配置每台设备只做一次，且尽量是开关 / 确认而非填写**。
WebDAV / S3 是唯一真要填的；后续支持从已配置端导出同步配置（二维码 / 配置文件），新端扫码
接入——N 次填写降为 1 次填写 + N 次扫码。`licenseKey` 在同步白名单里：激活一次全端生效。

跨生态（iOS↔Android、纯 Android 多机）只能靠 WebDAV / S3 适配器；这是全部方案里唯一的
网络代码，**纯客户端**，不是服务器。

## v1 迁移

目录里只有 v1 `payback.json` 且无任何分片：一次性导入——全部设备以本机 machineId 写进
自己的分片（`updatedAt` 沿用原值，`editedBy` = 本机），v1 文件原地保留作备份、此后不再写。
`version` 兼容读规则沿用 `data-format.md`。

## 测试门禁

- 合并语义黄金夹具进 `tests/`：union / LWW 决胜 / tombstone / 时钟偏差 / 半文件忽略 /
  扫描顺序无关 / 幂等。先写红（现有单文件实现必失败）再转绿。
- 夹具是数据文件而非 Racket 代码，**v2.0 的 Swift / Kotlin 核心跑同一份夹具**（RVT-lite
  一致性，防多语言核心行为漂移）。
- 新 RPC `sync-state`：同步目录、机器列表（id + name，来自各分片 meta）、上次合并时间、
  被忽略的文件清单。宿主据此渲染设置段状态。

## 落地拆步

| 步 | 内容 | 判据 |
|---|---|---|
| M1 ✅ 1.11.0 | 桌面三平台：store v2 分片 + `app/sync.rkt` 合并 + 各宿主「数据与同步」设置段 | 两台真机走任意网盘真实同步使用 |
| M2 | 随 v2.0：iOS iCloud 容器落点 + watchOS 只读 | TestFlight 双端收敛 |
| M3 | WebDAV（坚果云 / Nextcloud）→ S3 兼容（R2 / MinIO / OSS）适配器 | iOS↔Android 跨生态收敛 |
| M4 | 可选 E2EE：passphrase 派生钥（Argon2id + AEAD）分片各自加密、解密后合并；候选 Pro 卖点 | 第三方存储不可读验证 |

## 跨设备归属

`app/store.rkt`、新 `app/sync.rkt`、`app/wire.rkt`、`docs/data-format.md` 是共享层——改前按
跨设备协议在 #9 声明单写窗口；三宿主设置段 UI 各腿各自机器写（mac 本机 / Windows 腿与 #7
同批 / Linux 腿与 #8 同批）。
