# Changelog

## 1.1.0

The design pass — the app now looks like the product it is.

- Design language (docs/design.md): warm paper surface, Crail-orange
  accent, and a green reserved exclusively for paid-back states
- Summary strip: stat tiles with 「已赚回」 as the green hero tile
- Device cards: category-tinted icon tiles, gradient payback rings,
  milestone capsule chips, card shadows
- Detail sheet: gradient-filled daily-cost curve, ring + progress bar in
  the payback section, milestone ladder on card surfaces
- App icon: Crail coin with the falling cost curve and a payback check,
  injected into the macOS bundle by post-package.sh
- Window title is now Payback; Windows host aligned to the same palette
- Commercial path made public: PRICING.md (free core + planned Pro tier),
  EULA.md, COMMERCIAL-CHECKLIST.md; site gained a pricing note

## 1.0.1

- The built-in update source now points at this repository's GitHub
  Releases (`releases/latest/download`), so the in-app updater works out
  of the box. (1.0.0 shipped a placeholder host; its users can update by
  downloading the 1.0.1 installer manually — from 1.0.1 on, updates are
  automatic.)

## 1.0.0

First release. Payback is a native desktop app that turns gadget guilt into
arithmetic: record what you paid, watch the daily cost tick down, and set
what a day with the device is worth to see it pay itself back.

- Device records (name, emoji, category, price, purchase date, optional
  willingness-to-pay, notes) stored in one atomic, human-readable JSON file.
- Payback math in the Racket backend so macOS and Windows agree to the cent:
  daily cost, payback progress ring, break-even date, and the 「已赚回」
  library total.
- Milestone ladder: 100 / 365 / 1000 days together, daily cost under
  10 / 5 / 2 / 1 / 0.50, and fully paid back.
- Library summary: total spent, earned back, overall daily cost, device
  count; sorted by date added, daily cost, or payback progress.
- SwiftUI host on macOS (14+) and WinUI 3 host on Windows (10 19041+), both
  driven by one embedded-Racket-CS backend over RVT1.
- Signed online updates from day one: Ed25519-verified release manifests with
  key IDs and staged rollout, SHA-256-checked artifacts, size-capped
  downloads, per-platform installation (in-place app replacement with backup
  on macOS; transactional MSI on Windows), and a 24-hour-throttled,
  failure-silent launch check.
- zh / en UI from a single strings source with a committed generator and a
  CI parity check.
- 72 backend tests covering calendar edge cases, payback math, milestone
  boundaries, store recovery from corrupt files, wire validation, and the
  full update trust chain including manifest tampering.
