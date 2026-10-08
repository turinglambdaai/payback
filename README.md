# Payback

Watch every device pay itself back, one day at a time. Record what you paid, and Payback counts the daily cost down — set what a day with the device is worth to you, and watch the payback ring fill up.

[![CI](https://github.com/turinglambdaai/payback/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/payback/actions/workflows/ci.yml) ![macOS](https://img.shields.io/badge/macOS-SwiftUI-000000?logo=apple&logoColor=white) ![Windows](https://img.shields.io/badge/Windows-WinUI_3-0078D4?logo=windows11&logoColor=white) [![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

**English** · [中文](README.zh-CN.md) · 🌐 [payback.jrtx.site](https://payback.jrtx.site)


Every gadget purchase fights the same quiet guilt: *did I really need this?*
Payback answers with arithmetic instead of regret.

- **Daily cost** — `price ÷ days held`, ticking down every single day.
- **Payback progress** — record what one day with the device is worth to you
  (your 心理价位) and the app shows when it has officially paid itself back.
- **Earned back** — the sum of value gained beyond the price across your
  whole library. The number that turns impulse buys into investments.
- **Milestones** — 100 days together, daily cost under 1 yuan, fully paid
  back: small achievements, real dopamine.

Buy less, but also *use what you buy*. That is the whole product.

## Quick start

Payback is a first-party native desktop app built with
[Rivet](https://github.com/turinglambdaai/rivet): a Racket backend with an
embedded Racket CS runtime, SwiftUI on macOS and WinUI 3 on Windows. No
WebView, no cross-platform widget layer.

```bash
raco pkg install --auto rivet        # or a linked checkout of rivet
raco rivet doctor                    # check the native toolchain
raco rivet dev                       # build and run the current platform
```

Your devices live in a single JSON file (`docs/data-format.md` is the exact
contract): atomic writes, human-readable, easy to back up.

## Features

- Device records: name, emoji, category, price, purchase date, notes
- Daily cost with milestone ladder (100/365/1000 days · <10/5/2/1/0.50 per day)
- Payback progress ring and break-even date from your personal worth-per-day
- Library summary: total spent, earned back, overall daily cost
- Sort by date added, daily cost, or payback progress
- zh / en interface following the system language
- Free for up to 10 devices; **Payback Pro** lifts the cap with offline
  Ed25519 license keys — paste a token, no account, no call-home
- **Signed online updates from v1.0.0** — Ed25519-verified release manifests,
  SHA-256-checked artifacts, staged rollouts ([docs/updates.md](docs/updates.md))

## Building

```bash
raco test tests/          # 42 test cases + a full-protocol RPC run, 200+ assertions
raco rivet build          # backend + current platform native host
raco rivet package        # distributable .app / Windows directory, verified
node scripts/gen-strings.js --check   # UI strings parity (zh/en)
```

Windows host code is source-complete and builds on Windows with the usual
WinUI 3 toolchain (`raco rivet build` on Windows); CI exercises the Racket
backend on all three OSes.

## How it works

```text
              Racket backend (app/*.rkt)
              devices · payback math · updater
                        │
                   RVT1 protocol
                   ┌─────┴─────┐
               SwiftUI       WinUI 3
               macOS          Windows
```

All user-visible numbers are computed in Racket so both platforms agree to
the cent. The data model, RPC surface, milestone rules, and validation limits
are documented in [docs/data-format.md](docs/data-format.md); the update
trust chain in [docs/updates.md](docs/updates.md).

## Repository

```text
payback/
├── app/                  Racket backend (domain, store, wire, updater)
├── tests/                raco test suite (domain, store, wire, updater, RPC)
├── macos-host/           SwiftUI host (Swift Package)
├── windows/              WinUI 3 host (C++/WinRT)
├── shared/strings/       zh/en UI strings — single source, generated tables
├── scripts/              gen-strings.js · gen-update-keys.sh
├── docs/                 data-format.md · updates.md
└── rivet.rktd            app identity, version, deployment targets
```

## Honest gaps

- The Windows host is verified by CI on the backend side; pixel-level UI
  verification on real Windows hardware is still pending.
- On Windows the daily digest appears in the in-app status bar — unpackaged
  apps have no toast identity; macOS posts a real system notification.
- Single currency per library is the recommended flow; multi-currency totals
  add naively.
- No CSV export yet — the JSON file is the export, for now.

## Commercial path

The current version is free. The planned Pro tier (custom milestones, CSV
export, multiple ledgers) and the full commercial roadmap are public:
see [PRICING.md](PRICING.md), [COMMERCIAL-CHECKLIST.md](COMMERCIAL-CHECKLIST.md),
and the all-platform plan in [docs/platform-roadmap.md](docs/platform-roadmap.md).

## License

Licensed under the [MIT License](LICENSE).
