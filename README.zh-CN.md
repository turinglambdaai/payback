# Payback

每一台设备，都在一天天回本。记下购买价格，Payback 帮你算每日成本——再设定「这台设备一天值多少钱」，看回本进度一点点走满。

[English](README.md) · **中文** · 🌐 [payback.jrtx.site](https://payback.jrtx.site)

[![CI](https://github.com/turinglambdaai/payback/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/payback/actions/workflows/ci.yml) ![macOS](https://img.shields.io/badge/macOS-SwiftUI-000000?logo=apple&logoColor=white) ![Windows](https://img.shields.io/badge/Windows-WinUI_3-0078D4?logo=windows11&logoColor=white) [![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

## 安装

从 [Releases](https://github.com/turinglambdaai/payback/releases/latest) 下载：

| 平台 | 下载 | 更新 |
|---|---|---|
| macOS 14+ | `Payback-v<version>-macos.dmg` | 应用内更新（签名清单），或重新安装新版 DMG |
| Windows 10+ x64 | `payback-<version>-windows-x64.msi` | 应用内更新（签名清单） |

每个发布都带 `SHA256SUMS` 校验清单和 Sigstore 构建来源证明（`gh attestation verify <file> -R turinglambdaai/payback`）。

macOS 版本仅为 ad-hoc 签名。首次启动若被 Gatekeeper 拦截，右键应用选「打开」（或执行 `xattr -cr /Applications/Payback.app`）。

## 这个想法

每次剁手 electronics 之后，总有同一个安静的疑问：*我真的需要它吗？*
Payback 不安慰你，它给你算术。

- **每日成本** —— `价格 ÷ 持有天数`，每天都在往下走。
- **回本进度** —— 记下你愿意为它每天付的钱（心理价位），什么时候「正式回本」，进度条说了算。
- **已赚回** —— 全部设备超出价格的部分加在一起。这个数字把「又乱买了」变成「越用越赚」。
- **里程碑** —— 百日纪念、日均低于 1 元、完全回本：小小的成就，真实的 dopamine。

少买，更要**多用**。这就是全部产品逻辑。

## 快速开始

Payback 是用 [Rivet](https://github.com/turinglambdaai/rivet) 构建的原生桌面应用：Racket 后端 + 内嵌 Racket CS 运行时，macOS 上是 SwiftUI，Windows 上是 WinUI 3。没有 WebView，没有跨平台控件层。

```bash
raco pkg install --auto rivet        # 或链接本地 rivet checkout
raco rivet doctor                    # 检查原生工具链
raco rivet dev                       # 构建并运行当前平台
```

设备数据就是一个 JSON 文件（`docs/data-format.md` 是精确契约）：原子写入、人类可读、随手备份。

## 功能

- 设备记录：名称、emoji、分类、价格、购买日期、备注
- 每日成本 + 里程碑阶梯（100/365/1000 天 · 日均 <10/5/2/1/0.50 元）
- 回本进度环与预计回本日期（来自你自己的心理价位）
- 总览：累计投入、已赚回、整体日均
- 按加入时间 / 日均成本 / 回本进度排序
- 中英界面跟随系统语言
- **v1.0.0 起内置签名在线更新** —— Ed25519 验签发布清单、SHA-256 校验安装包、灰度放量（[docs/updates.md](docs/updates.md)）

## 构建

```bash
raco test tests/          # 72 个后端测试，含更新信任链
raco rivet build          # 后端 + 当前平台原生宿主
raco rivet package        # 可分发 .app / Windows 目录，含验证
node scripts/gen-strings.js --check   # 中英文案一致性
```

Windows 宿主源码完整，在 Windows 上用常规 WinUI 3 工具链构建（Windows 上执行 `raco rivet build`）；CI 在三大系统上验证 Racket 后端。

## 工作方式

```text
              Racket 后端 (app/*.rkt)
              设备 · 回本计算 · 更新器
                        │
                   RVT1 协议
                   ┌─────┴─────┐
               SwiftUI       WinUI 3
               macOS          Windows
```

所有用户可见的数字都在 Racket 里计算，两个平台精确到分都一致。数据模型、RPC 面、里程碑规则、校验上限见 [docs/data-format.md](docs/data-format.md)；更新信任链见 [docs/updates.md](docs/updates.md)。

## 仓库结构

```text
payback/
├── app/                  Racket 后端（domain / store / wire / updater）
├── tests/                raco test 测试（领域、存储、校验、更新器、RPC 协议）
├── macos-host/           SwiftUI 宿主（Swift Package）
├── windows/              WinUI 3 宿主（C++/WinRT）
├── shared/strings/       中英文案单源，生成各平台字符串表
├── scripts/              gen-strings.js · gen-update-keys.sh
├── docs/                 data-format.md · updates.md
└── rivet.rktd            应用标识、版本、部署目标
```

## 诚实的差距

- Windows 宿主的后端链路有 CI 覆盖；真机像素级 UI 验证还没做。
- 推荐单一货币记账；多币种汇总目前是简单相加。
- 还没有 CSV 导出——现阶段 JSON 文件本身就是导出。

## 商业路径

当前版本完全免费。规划中的 Pro 版（自定义里程碑、CSV 导出、多账本）与完整商业路径公开可查：见 [PRICING.md](PRICING.md) 与 [COMMERCIAL-CHECKLIST.md](COMMERCIAL-CHECKLIST.md)。全平台规划（含 iOS/watchOS/Android 的技术路线）：[docs/platform-roadmap.md](docs/platform-roadmap.md)。

## 许可

[MIT License](LICENSE)。
