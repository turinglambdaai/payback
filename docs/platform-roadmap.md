# Payback platform scope

**English** · [简体中文](platform-roadmap.zh-CN.md)

## Current scope

Maintain the macOS SwiftUI, Windows WinUI 3 and Linux GTK4 desktop hosts over the existing Racket domain and RVT1 contract. Published assets are previews; CI builds are not independent hardware/UI qualification. The public [user guide](USER-GUIDE.md) records current installation, data and privacy limitations.

## Next work

Keep the bounded sync queue in [#9](https://github.com/turinglambdaai/payback/issues/9) and its semantics in [sync-design.md](sync-design.md). Align free behavior in [#20](https://github.com/turinglambdaai/payback/issues/20), repair GTK date input in [#22](https://github.com/turinglambdaai/payback/issues/22), and group money by currency in [#23](https://github.com/turinglambdaai/payback/issues/23). These issues are acceptance work, not claims that existing builds are fixed.

## Deferred mobile work

No mobile or watch client is a current delivery or a sale. Evaluate demand and maintenance capacity only after desktop correctness and recovery. Runtime/embedding feasibility remains owned by [Rivet’s delivery plan](https://github.com/turinglambdaai/rivet/blob/main/docs/desktop-closeout-mobile-plan.md); do not introduce an ad-hoc RVT-lite protocol or duplicate the domain in Swift/Kotlin from this historical plan. Any future platform requires its own generated host, packaging, signing, device and product evidence.

## Commercial boundary

Every delivered platform is free; no mobile presale, device cap or sync/export paywall is planned. [PRICING.md](../PRICING.md) is the maintainer policy owner. The old all-platform buyout and independent mobile-core proposal are retired; their text remains in Git history.
