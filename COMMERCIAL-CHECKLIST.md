# Payback 商业化工程清单

> 对应 PRICING.md 的商业化路径。打勾 = 已就绪；其余按里程碑顺序推进。

## 已就绪（v1.0/1.1 完成的地基）

- [x] 分发通道：GitHub Releases 全自动（tag → DMG/MSI + 签名更新清单 + SHA256SUMS + provenance）
- [x] 应用内更新：Ed25519 签名清单 + 原生安装适配器（商业软件的最低门槛——用户能自动拿到新版本）
- [x] 更新签名密钥：主备份在 Sync/Keys（`payback-updater-private.der.age`），CI secret 注入
- [x] 产品主页 payback.jrtx.site（获客落点；待 Cloudflare 加 DNS 记录）
- [x] 双语 README + EULA + PRICING
- [x] 图标 + 设计语言（付费产品的基础观感）

## Pro 功能实现（v1.2 前完成）

- [x] **License 校验模块**（Ed25519，形态沿用 glaze/license 的离线方案）：
  - [x] `app/license.rkt`：公钥内嵌、PB1 令牌解析校验（字段：product/subject/type/key_id/expiry，到期日当天有效）
  - [ ] 私钥 `keys/license-ed25519-private.der` 入 Sync/Keys（已生成于本机 keys/，**待加密备份**；与更新密钥分开）
  - [x] RPC `activate-license` + `license-state`；授权存 settings.licenseKey（`license-state` 每次重验，到期自动降级）
  - [x] 发货工具 `scripts/issue-license.rkt`（私钥 + subject → PB1 令牌，打印进确认邮件即可）
  - [x] macOS「激活 Payback Pro…」菜单 + sheet；Windows 工具栏 Pro 按钮 + 对话框
- [x] **付费墙挂点**（首个付费点）：
  - [x] 设备数 >10 时添加拦截（免费版软限制 + 引导激活；已有库永不触碰）
  - [ ] 自定义里程碑编辑器（Pro，v3 数据模型升级时落地）
  - [ ] CSV/JSON 导出（Pro，同上）
  - [ ] 多账本（v3）
- [ ] 试用期：首次启动 14 天全功能（设置里记录 `trialStartedAt`）

## 收款与合规（发布 Pro 前）

- [ ] 国内收款：面包多/爱发电开店，商品 = Pro 授权码
- [ ] 海外收款：Gumroad 或 Lemon Squeezy（Merchant of Record，代缴欧盟 VAT）
- [ ] 授权码发货自动化：平台 webhook → 邮件发 PB1 令牌（短期可手动：`scripts/issue-license.rkt` 生成后粘贴进邮件）
- [ ] EULA 在应用首次启动展示一次（同意记录写设置）
- [ ] 定价页上 site（PRICING.md 内容改写为 HTML）
- [ ] 发票/收据说明页

## 增长（持续）

- [ ] jrtx.site 产品矩阵互导（Taskly/PodLens 页脚互链）
- [ ] 「回本挑战」小绿书内容系列（设备回本截图模板）
- [ ] Product Hunt / V2EX / 少数派发布帖（挂早鸟价）
- [ ] Obsidian/Notion 回本记账模板（借 JSON 格式生态获客）

## 达到收入后的升级项

- [ ] macOS Developer ID + 公证（消除右键打开；$99/年）→ `raco rivet release`（生产签名）
- [ ] Windows 代码签名证书（消除 SmartScreen 警告）
- [ ] 自动更新渠道加 beta 频道（rivet 原生支持 channel）
- [ ] 客服邮箱 + 常见问题页

## 红线（不变项）

- 数据永远是本机 JSON，不搞云锁定
- 免费版永久免费、无广告、无遥测（PRICING 承诺）
- 更新签名私钥与 license 私钥永不进仓库；一把锁一件事
