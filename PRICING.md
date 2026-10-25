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

Free / evaluation: No separate desktop/mobile charges, device-count gate, paid ledgers, export or BYO-sync tier. Mobile is a delivery plan, not a store-pricing plan. The legacy ten-device/PB1 gate is retired ([#20](https://github.com/turinglambdaai/payback/issues/20)); delivered releases are unrestricted and old data stays readable. The update-signing chain is preserved.

Paid: No paid tier is planned. Do not integrate Commerce or add Permit product gates.

## Authorization decision for implementation

Decision revision: `2026-10-auth-1` (2026-10-23). **No commercial Permit integration:** do not issue entitlement/trial/activation credentials, add Commerce checkout, impose device/seat limits or require a licensing account. Necessary third-party authentication, permissions and BYOK are separate from product licensing. Preserve update-signature verification and OS publisher trust; neither is a paid entitlement. Existing source licenses remain unchanged.

The conflicting legacy product gate has been removed through the linked implementation issue (#20); existing data opens, restores and exports unchanged. Record the policy/source SHA read at the next slice boundary. This decision is a development constraint, not proof that every future platform leg stays free by itself.

## Next gate before sales

#20 is complete: the legacy gate is retired. Free pricing does not reduce signed-update, recovery or privacy requirements.

Execution entry: [issue #20](https://github.com/turinglambdaai/payback/issues/20) (complete). No commercial expansion is planned; this section stays as policy history.

Portfolio role and shared boundaries: [PORTFOLIO](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/PORTFOLIO.md).
