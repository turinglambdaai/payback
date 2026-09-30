#!/usr/bin/env bash
# Apply scripts/post-package.sh to the .app inside an already-built release
# DMG and rebuild the DMG in place.
#
# `raco rivet release` builds its DMG from the freshly packaged app, before
# any post-package fixups (foreign libraries + consistent ad-hoc signature)
# can run. This script mounts that DMG, fixes the app, and produces a
# replacement DMG at the same path.
#
# Usage: scripts/fix-release-dmg.sh <path-to.dmg>

set -euo pipefail

DMG="${1:?usage: fix-release-dmg.sh <path-to.dmg>}"
[ -f "$DMG" ] || { echo "no such DMG: $DMG"; exit 1; }

HERE="$(cd "$(dirname "$0")" && pwd)"

WORK="$(mktemp -d /tmp/payback-dmg-fix-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

MOUNT="$WORK/mount"
mkdir -p "$MOUNT"
hdiutil attach -nobrowse -readonly -mountpoint "$MOUNT" "$DMG" >/dev/null
trap 'hdiutil detach "$MOUNT" -force >/dev/null 2>&1 || true; rm -rf "$WORK"' EXIT

APP_SRC="$(find "$MOUNT" -maxdepth 2 -name "*.app" -type d | head -1)"
[ -n "$APP_SRC" ] || { echo "no .app found inside $DMG"; exit 1; }

cp -R "$APP_SRC" "$WORK/"
APP_NAME="$(basename "$APP_SRC")"
"$HERE/post-package.sh" "$WORK/$APP_NAME"

hdiutil detach "$MOUNT" -force >/dev/null
rmdir "$MOUNT" 2>/dev/null || true

VOLNAME="$(basename "$DMG" .dmg)"
hdiutil create -volname "$VOLNAME" -srcfolder "$WORK/$APP_NAME" -ov -format UDZO "$DMG"
echo "fixed DMG: $DMG"
