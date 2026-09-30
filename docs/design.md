# Design language

Payback 的观感是产品价值的一部分：数字是主角，情绪回报靠视觉兑现。本文件是
两端 UI 的视觉契约；改动先改这里。

## 色板

| 语义 | 值 | 用途 |
|---|---|---|
| paper | `#FAF7F2`（暗色跟随系统） | 页面底色 |
| card | 系统控件底色（白/深灰） | 卡片表面，带轻微纵向渐变与投影 |
| accent | `#C15F3C`（Crail 橙） | 品牌色：进度环、主按钮、曲线、badge |
| accent-soft | accent 14% 透明 | badge 底、空状态、提示块 |
| earn | `#38964D`（绿） | **只**用于「已回本/已赚回」状态——绿色出现即是回报 |
| earn-soft | earn 14% 透明 | 回本后 hero 卡/进度区底色 |

规则：橙色 = 进行中的价值，绿色 = 已经兑现的回报。两者不混用。

## 字形与数字

- 所有金额/百分比用 rounded design + `monospacedDigit`，数字不跳动
- 大数字是主角：日均成本 title2 bold；「已赚回」为正时用 title2 绿色 hero 卡
- 辅助信息 caption + secondary，永远不与数字抢层级

## 部件

- 卡片：16pt 圆角（continuous），16pt 内边距，浅投影
- 回本进度环：5pt 描边、accent 渐变、圆头；100% 显示绿勾
- 里程碑：emoji + 文案胶囊（accent-soft 底）；卡片上最多展示 2 枚 + 计数
- 成本曲线：超衰减曲线，白/橙渐变描边 + 底部渐变填充；永不上扬
- 图标：暖米底 squircle + Crail 橙硬币 + 白色下行曲线 + 绿色回本对勾
  （`shared/assets/payback.icns`，经 `rivet.rktd` 的 `macos-icon` 由 rivet 打包注入）

## 实现位置

- macOS：`macos-host/Sources/RivetHost/PaybackTheme.swift`（色板/部件单源）
- Windows：`windows/MainWindow.xaml(.cpp)` 主题对齐（页面 `#FAF7F2`、白卡、
  发丝边框、圆角 12）
