# Payback Linux host

GTK4 window over the embedded Racket CS backend — same RVT1 contract as the
SwiftUI and WinUI hosts (`docs/data-format.md`). Plain C++ with GTK4 and
json-glib; no libadwaita dependency.

## Build

`raco rivet build` on Linux does everything (codegen, backend staging, CMake):
it needs the env contract from the embeddable Racket CS build —
`RIVET_ROOT`, `RIVET_RACKET_INCLUDE`, `RIVET_RACKET_LIBRARY` (static
`libracketcs.a`), `RIVET_RACKET_BOOT_DIR` — plus the system packages:

```bash
sudo apt-get install -y cmake ninja-build pkg-config \
  libgtk-4-dev libjson-glib-dev libsecret-1-dev libglib2.0-dev
raco rivet doctor      # validates the toolchain
raco rivet build       # → .rivet/stage/RivetHost (self-contained)
raco rivet package     # → dist/payback-linux-<arch>/
```

The standard Racket installer does not ship the embeddable static library;
build Racket CS from source once (CI caches it) — see
`.github/workflows/ci.yml` (`linux-host` job) for the exact recipe.

## Install (release)

Releases carry a self-contained `payback-<version>-linux-x64.tar.gz`:

```bash
tar -xzf payback-*-linux-x64.tar.gz
cd payback-linux-x64 && ./RivetHost
```

Data lives in `$XDG_DATA_HOME/payback` (default `~/.local/share/payback`),
the UI language choice in `~/.local/share/payback/ui-language.txt`. Updates
download and verify in-app; extract the new tar.gz over the app directory to
finish an update (docs/updates.md).
