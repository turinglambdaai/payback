#!/bin/sh
# Post-package fixups for the macOS bundle. Run after `raco rivet package`
# (and again after any repackage) before the app is distributed or wrapped
# into a DMG:
#
#   scripts/post-package.sh dist/payback.app
#
# 1. Copies Racket's foreign libraries (libgmp, libcrypto, libssl) into the
#    bundled runtime tree. The embedded backend needs them at runtime — the
#    crypto package behind rivet/distribution loads libgmp when the update
#    checker initializes — but the Racket.framework staging does not carry
#    them. The host resolves them relative to its Resources directory, which
#    it sets as its working directory at startup.
# 2. Re-signs the whole bundle ad hoc. `raco rivet package --development`
#    signs the framework and executable separately, which macOS (26+) rejects
#    with a Team-ID mismatch at launch; a single deep ad-hoc pass is
#    consistent and launches cleanly. Production releases signed with a real
#    Developer ID skip this step (pass --skip-codesign).

set -eu

APP="$1"
[ -d "$APP" ] || { echo "usage: $0 <path-to-.app> [--skip-codesign]"; exit 1; }

SKIP_CODESIGN=false
[ "${2:-}" = "--skip-codesign" ] && SKIP_CODESIGN=true

RACKET_BIN="$(readlink -f "$(command -v racket)")"
RACKET_LIB="$(cd "$(dirname "$RACKET_BIN")/../lib/racket" && pwd)"
[ -d "$RACKET_LIB" ] || { echo "post-package: cannot locate Racket lib dir from $RACKET_BIN"; exit 1; }

DEST="$APP/Contents/Resources/runtime/lib/plt/generic/exts/ert/r0"
mkdir -p "$DEST"

for pair in "libgmp.10.dylib:libgmp.10.dylib.10" \
            "libgmp.10.dylib:libgmp.10.dylib" \
            "libcrypto.3.dylib:libcrypto.3.dylib" \
            "libssl.3.dylib:libssl.3.dylib"; do
  src="${pair%%:*}"
  name="${pair##*:}"
  if [ -f "$RACKET_LIB/$src" ]; then
    cp "$RACKET_LIB/$src" "$DEST/$name"
  else
    echo "post-package: warning: $RACKET_LIB/$src not found" >&2
  fi
done

if [ "$SKIP_CODESIGN" = true ]; then
  echo "post-package: libraries copied (codesign skipped)"
else
  codesign --force --deep --sign - "$APP"
  echo "post-package: libraries copied and bundle re-signed: $APP"
fi
