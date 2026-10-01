# 平台路线图 — Rivet 全客户端战略

> 规划：payback 全面覆盖 Windows / macOS / Linux / iOS / iPadOS / watchOS / Android，
> 一切构建在 rivet 之上。本文件是对技术可行性与顺序的诚实分析（2026-09-30）。

## 现状

| 平台 | 状态 | 说明 |
|---|---|---|
| macOS 14+ | ✅ 生产 | SwiftUI 宿主，v1.1 起 |
| Windows 10+ | ✅ 生产（源码完整） | WinUI 3 宿主，CI 构建通过 |
| Linux | ⚠️ rivet 实验性 | GTK4 宿主已存在；缺 `raco rivet` CLI 集成、打包与 CI |
| iOS / iPadOS | ❌ | 见下文「移动端的硬约束」 |
| watchOS | ❌ | 依赖 iOS 端 |
| Android | ❌ | 同上 |

## 移动端的硬约束（必须先想清楚）

**Racket CS 依赖 JIT，而 iOS 全面禁止 JIT**（W^X 政策）。这意味着「把 Racket
核心嵌进 iOS 进程」这条桌面上的路，在 iOS 上**走不通**，也不是短期内能绕开的
（Chez 的软件解释模式性能与成熟度都不够；racket bc 无 JIT 但生态与编码器支持
边缘化）。Android 没有这个政策限制，但为一份业务逻辑维护两种嵌入技术不值得。

三条路摆过之后（网络伴随守护进程 / 无 JIT Racket / 协议兼容原生核心），
**推荐第三条**：

### 方案 A：RVT 协议兼容的「每平台原生核心」

- 桌面（macOS/Windows/Linux）：Racket 核心继续，rivet 的强项原样保留
- iOS / iPadOS：Swift 实现 `app/core`（payback 的领域逻辑是几百行纯函数——
  日期算法、日均/回本/里程碑/汇总，契约已在 `docs/data-format.md` 完全文档化）
- Android：Kotlin 同理
- watchOS：**只读**消费同步的 JSON——一个进度环 + 今日成本，实现成本极低、
  佩戴价值极高（手腕上的回本进度）
- 行为一致性靠**黄金测试锁死**：同一组输入/输出夹具跑 Racket 与 Swift/Kotlin
  两份实现（payback 的 tests/ 已具备夹具形态）

payback 恰好是这条路的最佳试点：领域逻辑小、纯函数、无平台 API 依赖。
Rivet 层面则沉淀出「移动端协议兼容客户端」的通用模式（RVT-lite：RPC 子集 +
JSON 契约 + 同步文件格式），后续每个 rivet 产品的移动端都复用。

## 顺序（建议）

| 阶段 | 内容 | 判据 |
|---|---|---|
| v1.x | 桌面三平台打磨：macOS ✅、Windows 真机验证（v1.1.1 跑通，像素级复核待做）、**Linux 完成 rivet CLI/打包集成**（反哺上游） | 三平台同版本发布 |
| v1.4 | JSON 同步故事：iCloud Drive / OneDrive / Resilio 任意文件夹即同步；最后写入胜出 + 冲突提示（原 v1.3 规划；实际 v1.3 交付了情绪价值版：快报/庆祝/语录） | 两台设备真实同步使用 |
| v2.0 | **iOS + iPadOS**（Swift 核心 + SwiftUI；RVT-lite 模式落地）+ watchOS 只读配套；Pro 开卖 | TestFlight 外部测试 |
| v2.x | **Android**（Kotlin 核心 + Compose） | 与 iOS 功能对齐 |
| v3 | 数据模型升级：转卖/折价、多账本、自定义里程碑 | Pro 功能兑现 |

## 商业模式（全平台版）

- **一次买断，全平台通用**：¥68（早鸟 ¥45）覆盖 macOS/Windows/Linux/iOS/
  iPadOS/watchOS/Android——「你的每一块屏幕上都有回本进度」。平台数量是
  涨价理由：每新增一个平台，价格上调一档，早买者永久受益。
- **同步即增值**：本地优先不变——多端同步走用户自己的网盘文件夹
  （iCloud/OneDrive/Resilio），我们不建服务器、不碰用户数据；「设备间无缝」
  这个体验本身就是 Pro 的核心付费点。
- **免费版全端一致**：≤10 台 + 核心情绪循环完整，任何一端下载即完整体验
  （获客漏斗不因平台收窄）。
- **收入结构**：一次性授权为主；后续可选「订阅制家庭共享」作为第二档
  （家庭多账本场景），不作为默认。

## 风险

- iOS 审核对「文件同步 + 外部购买」敏感：license 走应用外销售（网站），
  iOS 端只做「输入授权码」解锁，不走 IAP 也可过审（需保留 IAP 备选方案）
- Swift/Kotlin 核心与 Racket 核心的行为漂移：黄金夹具必须进 CI（每端跑同
  一份夹具文件），漂移即红
- Linux 桌面市场小：rivet Linux 路径的完成度以「payback 能发」为界，不追
  发行版打包全覆盖（tar/AppImage 起步）
