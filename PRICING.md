# Payback product and pricing plan

**English** · [简体中文](PRICING.zh-CN.md)

## Launch status and implementation boundaries

- Policy version: `2026-10-v2`, 2026-10-23. The owner confirmed that every product is still in development and has no actual users. This document replaces conflicting previous pricing, early-bird and Pro proposals; there is no existing customer cohort to migrate. **Prices are launch targets, not live offers or claims that the listed features are implemented.**
- Entitlements cover delivered platforms only. New commercial plans do not change the LICENSE of released source. Source use, modification and redistribution rights are separate from official products, updates and services.
- Security fixes and reading, backing up or exporting your own data must not require another purchase. BYOK keys, payment-provider secrets and issuer private keys have different owners. Native hosts do not collect payment or issue credentials directly.

## Decision

**Free on every delivered platform; community entry point**

Launch target: **¥0 / US$0**. CNY and USD are independently set regional targets, not a live exchange-rate conversion.

Keep billing and support complexity out of small tools. Build community trust through usable products and concentrate revenue work on a few paid products.

## Free and paid scope

Free / evaluation: No separate desktop/mobile charges, device-count gate, paid ledgers, export or BYO-sync tier. Mobile is a delivery plan, not a store-pricing plan. Current code still has a legacy ten-device/PB1 gate: remove it through #20 before claiming unrestricted releases. Preserve old-data compatibility and the update-signing chain.

Paid: No paid tier is planned. Do not integrate Commerce or add Permit product gates.

## Next gate before sales

Complete delivery/maintenance gates in #20 first. Free pricing does not reduce signed-update, recovery or privacy requirements.

Execution entry: [issue #20](https://github.com/turinglambdaai/payback/issues/20). Complete this bounded slice before expanding commercial features.

Portfolio role and shared boundaries: [PORTFOLIO](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/PORTFOLIO.md).
