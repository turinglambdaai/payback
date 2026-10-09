# Changelog

## 1.4.1

Fixes the macOS in-app update flow, which shipped in 1.4.0 unable to
complete a check or a download:

- **Manifest and artifact fetches follow redirects**: release assets
  answer with a 302 to their CDN, and the updater's plain HTTP fetch
  verified an empty redirect body — every update check failed at the
  signature step (fix mirrored upstream in rivet#153)
- **The available state is a real phase**: a successful check with an
  update present now shows version, size, and a 下载更新 button; 1.4.0
  rendered "✅ up to date" for an available update and nothing ever
  started the download (the install adapter itself was fine)
- The consent-first flow matches Windows: available → download with
  progress → quit and install

## 1.4.0

Linux joins the family, and the paid-product foundation lands:

- **Linux support**: a first-party GTK4 host over the embedded Racket
  backend — the full product, three-platform parity at last; releases
  now ship a Linux tar.gz alongside the DMG and MSI
- **License foundation**: offline PB1 tokens (Ed25519-signed, no
  account, no call-home), activate/license-state RPCs, a 10-device free
  wall, and the Payback Pro activation UI on macOS and Windows
- **Consent-based updates on Windows**: explicit dialogs gate every step
  (available → download with progress → quit to install); "Date added"
  now sorts by true creation order and every user-visible string routes
  through generated Strings.h
- **Drag-to-Applications DMG on macOS**: the installer image stages an
  Applications drop target, and in-app updates no longer re-trigger
  Gatekeeper (quarantine xattr cleared after the verified copy)
- Backend: shape-validated store, read-only load-all, deeper test
  coverage; distribution stays GitHub Releases only (winget support
  dropped)

## 1.3.0

The emotional-value release: 有用、有趣、好看.

- **回本快报**：once per day, a signed-system notification reports what
  the library earned and today's cheapest device (permission denial is
  remembered and never nagged; every failure stays silent)
- **里程碑庆祝**：achievements reached since the last launch trigger a
  confetti celebration sheet (ack-on-read — each milestone is celebrated
  exactly once)
- **回本语录**：a rotating daily quip under the summary strip
- Progress rings animate in; celebration quips in both languages ride
  the shared strings source
- docs/data-format.md: seenMilestones/lastDigestAt settings, milestone
  new flag, daily-digest RPC

## 1.2.0

Two things users immediately look for in a paid-product candidate:

- **In-app language switching**: a globe menu in the toolbar offers
  跟随系统 / 中文 / English, applied live across the whole UI without a
  restart; the choice persists and overrides the system language
  (Windows host gets the same switcher, source-complete)
- **Dark mode verified end to end**: every surface uses semantic system
  colors, so the entire app — cards, rings, badges, sheets — adapts when
  the system switches appearance (verified by screenshot in both modes);
  the Windows host gains ThemeDictionaries and theme-aware card surfaces
- Product homepage gains a dark scheme (prefers-color-scheme)
- Strategy made public: docs/platform-roadmap.md lays out the all-client
  plan (Windows/macOS/Linux → iOS/iPadOS/watchOS/Android) including the
  honest Racket-on-iOS constraint and the RVT-compatible native-core
  approach; PRICING.md shifts to one universal license across platforms

## 1.1.1

- Windows: fix the startup crash (access violation in RivetHost.exe).
  The sort ComboBox raises `SelectionChanged` while `InitializeComponent`
  is still binding `x:Name` members, and the handler touched the
  not-yet-bound device list panel; the render is now guarded until the
  window is ready. This is the first release whose Windows host was
  verified running on real hardware, not just compiled.
- README: added a user-facing Install section; the repository now links
  payback.jrtx.site from its About panel.

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
