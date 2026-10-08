# AGENTS.md

指引给 AI agent（及开发者）：如何理解、构建、改动 Payback。

## 这是什么

Payback 是一个「设备回本」桌面应用：记录电子产品的购买价格，计算每日使用成本，越用越便宜；用户为设备设定「每天愿意付的心理价位」后，还能看到回本进度条和「已赚回」总额。核心情绪价值：**用得越久，越觉得赚了。**

技术形态遵循 [Rivet](https://github.com/turinglambdaai/rivet) 架构：**Racket 后端（内嵌 Racket CS 运行时）+ 各平台第一方原生 UI**，二者通过 RVT1 协议（typed RPC）通信，不共享运行时代码，靠 `docs/data-format.md` 契约保持一致。

| 平台 | 技术栈 | 目录 | 状态 |
|---|---|---|---|
| macOS 14+ | SwiftUI（Swift Package） | `macos-host/` | ✅ 构建+测试+package 验证通过 |
| Windows 10+ | WinUI 3（C++/WinRT） | `windows/` | 源码完整；v1.1.1 起真机验证过，像素级复核待做 |
| Linux x64 | GTK4（C++，json-glib） | `linux/` | CI 构建 + xvfb 启动冒烟；真机复核待做 |

## 快速命令

```bash
raco test tests/                      # 42 个测试用例 + 全协议 RPC 运行（必须全绿）
# Linux 宿主另需 GTK4/json-glib dev + 内嵌 Racket CS（配方见 CI linux-host job）
raco rivet build                      # 编译后端 + 当前平台宿主（生成 typed 客户端）
raco rivet dev                        # 构建并运行当前平台
raco rivet package                    # 打包 + 审计验证
node scripts/gen-strings.js --check   # 中英文案一致性校验
```

## Rivet 前置流程（重要）

Rivet 迭代很快：**每次开发 payback 前，先把本地 rivet 更新到最新**——

```bash
cd ../rivet && git pull origin main    # 本地是 link 安装，pull 即更新
raco test tests/ && raco rivet build   # 回归 payback
```

遇到 rivet 自身的问题**直接提 issue 或 PR** 到 turinglambdaai/rivet（已提：#91 macOS 宿主工作目录修复）。本地 rivet 保持在 main 分支。

macOS 打包：`raco rivet package` 产物已自包含（外部库由 `raco ctool` staging 进 `runtime/lib`，宿主启动时切工作目录到 Resources 解析；图标走 `rivet.rktd` 的 `macos-icon`）——**无需任何后处理脚本**（旧 post-package.sh 已随 rivet #81/#91 上游修复退役）。

## 契约文档（改任何行为前必读，改动必须同步契约）

| 文档 | 内容 |
|---|---|
| `docs/data-format.md` | JSON 存储布局、设备记录字段、computed 块（日均/回本/里程碑）、summary、RPC 面、校验上限 |
| `docs/updates.md` | 更新信任链（Ed25519 清单→SHA-256 工件→原生安装）、发布流程、密钥轮换 |
| `shared/strings/strings.json` | 全部用户可见文案（zh/en 单源） |
| `rivet.rktd` | 应用标识、版本、build 号、部署目标 |

## 改动规则（不要破坏）

- **领域计算只在 Racket**（`app/domain.rkt`）：日均成本、回本进度、里程碑、汇总。原生宿主只渲染，不计算。日期算法是 Howard Hinnant 的 `days_from_civil`，购买当天算第 1 天（`daysHeld = diff + 1`），改前先跑 `tests/domain-test.rkt`
- **内部 JSON 全部符号键**：Racket `read-json`/`write-json` 的键是 symbol；线上 JSON 形状不变（camelCase 字段名）
- **可选值用 `'null`** 不用 `#f`：`#f` 会在线上变成 JSON `false`，`'null` 才是 null
- **里程碑 key 是稳定标识**（`days-100`、`cpd-1`、`paid-back`…）：各平台本地化标签，后端只发 key + achieved
- **校验错误信息是用户可见的**：原生宿主原样展示，写给最终用户看
- **更新器永不崩溃**：检查失败静默（自动检查不弹窗）、下载失败可重试、损坏数据文件移到 `.corrupt-<ts>` 重新开始
- **版本双写**：`rivet.rktd` 和 `app/version.rkt` 必须同一 release 提交内一致
- **文案改 `strings.json` 后跑 `node scripts/gen-strings.js`** 并提交再生成的 `L10n.swift`/`Strings.h`，不要手改这两个文件

## 更新签名密钥

`keys/update-ed25519-private.der` 永不入库（gitignore）；公钥 base64 内嵌在 `app/version.rkt`。生成/轮换用 `scripts/gen-update-keys.sh`。发布时 CI 走 secret `UPDATE_ED25519_PRIVATE_KEY_B64`；本地走环境变量（见 docs/updates.md）。主备份在 Sync/Keys 密钥库（`payback-updater-private.der.age` + `payback-keys.README.md`）。

**License 签名密钥**（与更新密钥分开，一把锁一件事）：`keys/license-ed25519-private.der` 永不入库；公钥 base64 内嵌在 `app/version.rkt`（`license-public-key-b64`）。生成用 `scripts/gen-license-keys.sh`，给客户发授权码用 `scripts/issue-license.rkt`（输出 PB1 令牌，粘贴进应用激活对话框即完成离线验证）。轮换规则同更新密钥：先发信任新公钥的版本，再换签发钥。

**Racket crypto 坑**：ed25519 私钥的 `rkt-private` datum 元素顺序不固定（DER 导入是 `(vk sk)`，新生成是 `(sk vk)`），公钥一律用 `pk-key->datum priv 'rkt-public` 派生，禁止按下标取元素。

## 项目结构

```text
payback/
├── app/                  Racket 后端：domain(纯计算) / store(JSON 持久化) /
│                         wire(校验) / license(PB1 令牌) / updater(签名更新) /
│                         version(常量) / backend(RPC 装配)
├── tests/                domain / store / wire / license / updater / rpc（协议级全链路）
├── macos-host/           SwiftUI 宿主：RivetHostApp(模型) / ContentView(列表+总览) /
│                         DeviceForm / DeviceDetail / UpdateView(含安装适配器) /
│                         Models(wire 模型) / L10n(生成) / Money
├── windows/              WinUI 3 宿主：MainWindow.xaml(.h/.cpp) + MainWindow.Update.cpp
│                         (更新/快报/庆祝) + HostHelpers(共享工具) + Strings.h(生成)
├── linux/                GTK4 宿主：src/main.cpp（单文件全流程）+ Strings.h(生成)
├── shared/strings/       strings.json 文案单源
├── scripts/              gen-strings.js（生成+校验）· gen-update-keys.sh
├── docs/                 data-format.md · updates.md
├── keys/                 更新签名公钥（私钥 gitignore）
└── rivet.rktd
```

## 常见任务

- **加字段**：先改 `docs/data-format.md` → `app/wire.rkt`（校验）→ `app/domain.rkt`（如有计算）→ Swift `Models.swift` + C++ `DeviceRow` → 测试
- **加 RPC**：`app/backend.rkt` 里 `define-rpc` → 重新 `raco rivet build` 生成两端客户端 → 在 Swift/C++ 调用 → `tests/rpc-test.rkt` 补协议测试
- **改文案**：只改 `shared/strings/strings.json` + 跑生成器
- **改里程碑规则**：`app/domain.rkt` 的 ladder + `docs/data-format.md` + 两端 `MilestoneBadge`/`milestone_*` 本地化
- **发布**：打 `vX.Y.Z` tag 推送即可，CI 全自动出 DMG/MSI + 签名清单 + GitHub Release（见 docs/updates.md 和 .github/workflows/release.yml）
