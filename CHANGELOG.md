# Changelog

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
