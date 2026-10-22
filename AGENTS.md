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
raco rivet schema check rivet-schema.json --json # RPC 兼容性门禁
# Linux 宿主另需 GTK4/json-glib dev + 内嵌 Racket CS（配方见 CI linux-host job）
raco rivet build                      # 编译后端 + 当前平台宿主（生成 typed 客户端）
raco rivet dev                        # 构建并运行当前平台
raco rivet package                    # 打包 + 审计验证
raco rivet acceptance --json          # 当前平台打包产品验收
node scripts/gen-strings.js --check   # 中英文案一致性校验
```

## 完成定义

- GUI 功能必须通过 `raco rivet acceptance --json`：领域契约、真实 RVT1、
  原生宿主、可分发包以及打包后的原生 UI 操作缺一不可。
- 默认验收在隔离数据目录中植入 `Codex Acceptance Device`，启动打包应用，
  并确认该设备经后端计算后进入原生可访问性树；空窗口、截图或构建成功不算完成。
- 跨平台交互要同步 macOS、Windows、Linux，或明确记录真实平台边界。
- 不得用 `continue-on-error`、空测试、跳过 UI 或强杀进程作为成功证据。

## Rivet 前置流程（重要）

Rivet 迭代很快：**每次开发 payback 前，先把本地 rivet 更新到最新**——

```bash
cd ../rivet && git pull origin main    # 本地是 link 安装，pull 即更新
raco test tests/ && raco rivet build   # 回归 payback
```

遇到 rivet 自身的问题**直接提 issue 或 PR** 到 turinglambdaai/rivet（已提：#91 macOS 宿主工作目录修复）。本地 rivet 保持在 main 分支。

macOS 打包：`raco rivet package` 产物已自包含（外部库由 `raco ctool` staging 进 `runtime/lib`，宿主启动时切工作目录到 Resources 解析；图标走 `rivet.rktd` 的 `macos-icon`）——**无需任何后处理脚本**（旧 post-package.sh 已随 rivet #81/#91 上游修复退役）。

Windows 打包（v1.4.1 两个发版事故的教训）：**后端 crypto 只许走 `rivet/distribution`，禁止 require `crypto/all`**——nettle factory 的 GMP FFI 在模块加载期硬性 `ffi-lib`，宿主机器没有 libgmp 就启动即死；rivet 负责把 OpenSSL/GMP DLL stage 进包（rivet #204 起）。图标走 `rivet.rktd` 的 `windows-icon`（.ico 进 exe 资源 + MSI ARPPRODUCTICON）。**平台宿主按包消费 rivet（rivet #238）**：`windows/RivetHost.vcxproj` 用 `ProjectReference` 指向 `rivet-host.vcxproj`、`linux/CMakeLists.txt` 用 `add_subdirectory` + `rivet::platform`、`macos-host/Package.swift` 依赖 rivet 根包——宿主不再把 rivet 源文件编进自己的构建（rivet 加编译单元无需下游改动）；Windows 别丢 `RIVET_WINDOWS_ICON_RC` 资源编译块。CI/release 的 rivet 安装按 `RIVET_PIN` 单源锁定；`setup-embed-racket` action 的 `uses:` 是字面 pin（GitHub Actions 不展开 env），升级时须与 `RIVET_PIN` 联动改两处（两处均有 keep-in-sync 注释）。

品牌安装器资源：`rivet.rktd` 的 `dmg-background`（`shared/assets/dmg-background.png`，1320×800 即 660×400@2x）与 `windows-installer-bitmaps`（`wizard-banner.bmp` 493×58 / `wizard-dialog.bmp` 493×312，24 位 BMP），SVG 源与 PNG 预览同目录。当前 pin v0.12.0 已消费这些品牌安装器资源。**不要当作未使用资源删除。**

## 契约文档（改任何行为前必读，改动必须同步契约）

| 文档 | 内容 |
|---|---|
| `docs/data-format.md` | JSON 存储布局、设备记录字段、computed 块（日均/回本/里程碑）、summary、RPC 面、校验上限 |
| `docs/sync-design.md` | 多端同步契约（数据格式 v2）：BYO 文件夹零服务器、机器分片单写者、tombstone、记录级 LWW、settings 同步白名单、监听/触发、平台落点、M1–M4 拆步。**尚未实施**，进度在 #9 |
| `docs/UPDATE.md` | 更新信任链（Ed25519 清单→SHA-256 工件→原生安装）、平台×架构矩阵、发布流程、密钥轮换 |
| `shared/strings/strings.json` | 全部用户可见文案（zh/en 单源） |
| `rivet.rktd` | 应用标识、版本、build 号、部署目标 |

## 改动规则（不要破坏）

- **领域计算只在 Racket**（`app/domain.rkt`）：日均成本、回本进度、里程碑、汇总。原生宿主只渲染，不计算。日期算法是 Howard Hinnant 的 `days_from_civil`，购买当天算第 1 天（`daysHeld = diff + 1`），改前先跑 `tests/domain-test.rkt`
- **内部 JSON 全部符号键**：Racket `read-json`/`write-json` 的键是 symbol；线上 JSON 形状不变（camelCase 字段名）
- **可选值用 `'null`** 不用 `#f`：`#f` 会在线上变成 JSON `false`，`'null` 才是 null
- **里程碑 key 是稳定标识**（`days-100`、`cpd-1`、`paid-back`…）：各平台本地化标签，后端只发 key + achieved
- **校验错误信息是用户可见的**：原生宿主原样展示，写给最终用户看
- **更新器永不崩溃**：检查失败静默（自动检查不弹窗）、下载失败可重试、损坏数据文件移到 `.corrupt-<ts>` 重新开始
- **同步/存储改动遵循 `docs/sync-design.md`**：按机器分片、每台机器只写自己的分片、删除用 tombstone、记录级 `(updatedAt, editedBy)` LWW。禁止整文档 LWW、禁止在单文件 `payback.json` 上叠同步、禁止同步数据库二进制文件、不建服务器
- **版本三写**：根目录 `VERSION`、`rivet.rktd`、`app/version.rkt` 必须同一 release 提交内一致，`scripts/check-release-version.sh`（CI + release 每个跑打包的 job 都会执行）负责把关
- **文案改 `strings.json` 后跑 `node scripts/gen-strings.js`** 并提交再生成的 `L10n.swift`/`Strings.h`，不要手改这两个文件

## 更新签名密钥

`keys/update-ed25519-private.der` 永不入库（gitignore）；公钥 base64 内嵌在 `app/version.rkt`。生成/轮换用 `scripts/gen-update-keys.sh`。发布时 CI 走 secret `UPDATE_ED25519_PRIVATE_KEY_B64`；本地走环境变量（见 docs/UPDATE.md）。主备份在 Sync/Keys 密钥库（`payback-updater-private.der.age` + `payback-keys.README.md`）。

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
├── scripts/              gen-strings.js（生成+校验）· gen-update-keys.sh · check-release-version.sh · make-update-manifest.sh
├── docs/                 data-format.md · sync-design.md · UPDATE.md · design.md · platform-roadmap.md
├── keys/                 更新签名公钥（私钥 gitignore）
└── rivet.rktd
```

## 常见任务

- **加字段**：先改 `docs/data-format.md` → `app/wire.rkt`（校验）→ `app/domain.rkt`（如有计算）→ Swift `Models.swift` + C++ `DeviceRow` → 测试
- **加 RPC**：`app/backend.rkt` 里 `define-rpc` → 重新 `raco rivet build` 生成两端客户端 → 在 Swift/C++ 调用 → `tests/rpc-test.rkt` 补协议测试
- **改文案**：只改 `shared/strings/strings.json` + 跑生成器
- **改里程碑规则**：`app/domain.rkt` 的 ladder + `docs/data-format.md` + 两端 `MilestoneBadge`/`milestone_*` 本地化
- **做多端同步（v1.4）**：先读 `docs/sync-design.md`，按 M1 拆步（store v2 分片 → `app/sync.rkt` 合并 → `sync-state` RPC → 各宿主「数据与同步」设置段），合并黄金夹具先红后绿；共享层改动前在 #9 声明单写窗口
- **发布**：确认 `VERSION` / `rivet.rktd` / `app/version.rkt` / CHANGELOG 四处一致后打 `vX.Y.Z` tag 推送，CI 全自动出三平台安装包 + 便携 zip + 签名清单 `update-manifest.json` + GitHub Release（见 docs/UPDATE.md 和 .github/workflows/release.yml）

## 组合定位（Portfolio placement）

- 轨道与授权：轨道 4 社区引流；授权形态：MIT；商业归属：永久免费。
- 组合归属唯一权威：[PORTFOLIO.md](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/PORTFOLIO.md)（turinglambdaai.github.io 仓库）。改归属/授权先改权威档，再同步本节。
- 纪律：价格只进本仓 `PRICING.md`，且收费开始时才发布；README 与产品站不做价格预告。
