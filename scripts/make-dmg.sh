#!/bin/sh
# Build the Payback drag-to-install DMG.
#
# Usage: scripts/make-dmg.sh <app-dir> <output.dmg> [volname]
#   <app-dir>    directory containing the packaged .app (raco rivet package
#                output); the first .app found is used
#   <output.dmg> DMG path to write
#   [volname]    volume name (default: Payback)
#
# The DMG always carries a link to /Applications next to the app so the
# install is drag-and-drop. When create-dmg (brew) is available the icons
# are positioned and the window sized; otherwise the plain hdiutil layout
# with the symlink is produced. First-launch Gatekeeper friction is a
# notarization question, not a DMG question — see COMMERCIAL-CHECKLIST.md.

set -eu

APP_DIR="${1:?usage: make-dmg.sh <app-dir> <output.dmg> [volname]}"
OUT="${2:?usage: make-dmg.sh <app-dir> <output.dmg> [volname]}"
VOLNAME="${3:-Payback}"

APP_SRC="$(find "$APP_DIR" -maxdepth 1 -name '*.app' -print -quit)"
if [ -z "$APP_SRC" ]; then
  echo "make-dmg: no .app bundle in $APP_DIR" >&2
  exit 1
fi

STAGING="$(mktemp -d /tmp/payback-dmg-XXXXXX)"
trap 'rm -rf "$STAGING"' EXIT

# ship as Payback.app regardless of how raco rivet package named it;
# the updater's install target is /Applications/Payback.app
cp -R "$APP_SRC" "$STAGING/Payback.app"

if command -v create-dmg >/dev/null 2>&1; then
  # app left, Applications drop target right
  create-dmg \
    --volname "$VOLNAME" \
    --window-size 640 380 \
    --icon-size 128 \
    --icon "Payback.app" 170 200 \
    --app-drop-link 470 200 \
    --hide-extension "Payback.app" \
    "$OUT" "$STAGING"
else
  ln -s /Applications "$STAGING/Applications"
  hdiutil create -volname "$VOLNAME" -srcfolder "$STAGING" \
    -ov -format UDZO "$OUT"
fi
