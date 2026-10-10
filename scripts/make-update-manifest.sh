#!/usr/bin/env bash
# Build the Payback update feed for a release and sign it — the family
# single-file wrapper (taskly baseline, shared spec: taskly
# shared/spec/UPDATE.md).
#
# Usage: scripts/make-update-manifest.sh <tag> <dist-dir> <key-der-path>
#   <tag>          release tag, e.g. v1.5.0 (must match the VERSION file,
#                  rivet.rktd, and app/version.rkt — release jobs run
#                  scripts/check-release-version.sh before packaging)
#   <dist-dir>     directory containing the release artifacts, i.e. the
#                 names the release pipeline produces:
#                   payback-<version>-macos-arm64.dmg
#                   payback-<version>-macos-x64.dmg
#                   payback-<version>-windows-x64.msi
#                   payback-<version>-linux-x64.tar.gz
#   <key-der-path> Ed25519 private key in DER (OneAsymmetricKey) form; the
#                  CI secret stores it base64-encoded.
#
# Emits <dist-dir>/update-manifest.json — a single self-contained signed
# wrapper (schema + base64 payload + signature block) carrying every
# platform × architecture — plus <dist-dir>/SHA256SUMS over all files.
#
# Feed policy (differs from taskly on purpose): payback's update
# semantics are installer-based (macOS mounts the DMG, Windows runs the
# MSI), so the feed carries the dmg/msi/targz installers. The portable
# .zip assets are for humans, not for the feed. The native Linux
# installers (deb/rpm/AppImage, release assets since 1.6.0) are installer
# assets too — package-manager installs upgrade through the package
# manager and AppImage installs replace the file — so the feed keeps
# carrying only the Linux tar.gz. The manifest is ALSO
# written to update-stable.json (byte-identical copy): pre-1.5.0 clients
# fetch the feed under that old channel name from
# releases/latest/download, and the signature covers the payload, not
# the file name, so they keep updating transparently. Drop the alias
# once 1.4.x installs are gone from the field.
#
# Env overrides: RELEASE_ASSET_BASE_URL, RIVET_UPDATE_KEY_ID.

set -euo pipefail

TAG="${1:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
DIST="${2:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
KEY_PATH="${3:?usage: make-update-manifest.sh <tag> <dist-dir> <key-der-path>}"
VERSION="${TAG#v}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY_ID="${RIVET_UPDATE_KEY_ID:-payback-2026-09}"
BASE_URL="${RELEASE_ASSET_BASE_URL:-https://github.com/turinglambdaai/payback/releases/download/$TAG}"

# ---- verify tag/version alignment -------------------------------------------
[[ "$VERSION" == "$(tr -d '[:space:]' < "$ROOT/VERSION")" ]] || {
  echo "error: tag $TAG does not match VERSION '$(cat "$ROOT/VERSION")'" >&2; exit 1; }

for artifact in "$DIST/payback-$VERSION-macos-arm64.dmg" \
                "$DIST/payback-$VERSION-macos-x64.dmg" \
                "$DIST/payback-$VERSION-windows-x64.msi" \
                "$DIST/payback-$VERSION-linux-x64.tar.gz"; do
  [[ -f "$artifact" ]] || { echo "error: missing $artifact" >&2; exit 1; }
done

# ---- build + sign the merged manifest with rivet's own signer ---------------
MANIFEST="$DIST/update-manifest.json"

SCRIPT="$(mktemp /tmp/payback-manifest-XXXXXX.rkt)"
trap 'rm -f "$SCRIPT"' EXIT

cat > "$SCRIPT" <<RKT
#lang racket/base
(require rivet/distribution
         racket/date
         racket/file
         racket/format)
(define version "$VERSION")
(define base-url "$BASE_URL")
(define dist (path->complete-path "$DIST"))
(define key-path (path->complete-path "$KEY_PATH"))
(define key-id "$KEY_ID")
(define build (hash-ref (file->value (build-path (path->complete-path "$ROOT") "rivet.rktd")) 'build))

(define (artifact platform architecture file installer)
  (define path (build-path dist file))
  (unless (file-exists? path)
    (error 'make-update-manifest "missing installer: ~a" path))
  (update-artifact platform architecture
                   (string-append base-url "/" file)
                   (sha256-file/hex path)
                   (file-size path)
                   installer
                   '()))

;; Installer-based feed: dmg entries for both macOS architectures, the MSI
;; for Windows, the tar.gz for Linux. Portable zips are human assets only.
(define manifest
  (update-manifest "site.jrtx.payback"
                   version
                   build
                   'stable
                   ;; published-at: RFC 3339, second precision, UTC
                   (let ([d (seconds->date (current-seconds) #f)])
                     (format "~a-~a-~aT~a:~a:~aZ"
                             (date-year d)
                             (~r (date-month d) #:min-width 2 #:pad-string "0")
                             (~r (date-day d) #:min-width 2 #:pad-string "0")
                             (~r (date-hour d) #:min-width 2 #:pad-string "0")
                             (~r (date-minute d) #:min-width 2 #:pad-string "0")
                             (~r (date-second d) #:min-width 2 #:pad-string "0")))
                   "0.0.0"
                   #f
                   #t
                   100
                   (list (artifact 'macos 'arm64
                                   (format "payback-~a-macos-arm64.dmg" version) 'dmg)
                         (artifact 'macos 'x64
                                   (format "payback-~a-macos-x64.dmg" version) 'dmg)
                         (artifact 'windows 'x64
                                   (format "payback-~a-windows-x64.msi" version) 'msi)
                         (artifact 'linux 'x64
                                   (format "payback-~a-linux-x64.tar.gz" version) 'targz))))

;; write-signed-manifest validates the struct against the manifest schema
;; before signing, so a malformed manifest fails the release instead of
;; shipping something every client would reject.
(call-with-output-file (build-path dist "update-manifest.json")
  #:exists 'truncate/replace
  (lambda (out)
    (write-signed-manifest manifest
                           (read-ed25519-private-key key-path)
                           key-id
                           out)
    (newline out)))
(printf "manifest: ~a (4 artifacts, key-id ~a)\\n"
        (build-path dist "update-manifest.json") key-id)
RKT

# rivet must be installed for the signer; the release job links a checkout
racket "$SCRIPT"

# Compat alias for pre-1.5.0 clients (see header): same signed bytes under
# the old channel-manifest name they still fetch.
cp "$MANIFEST" "$DIST/update-stable.json"

# ---- checksums (paths relative to the dist dir, so --check works from it) ----
( cd "$DIST" && find . -maxdepth 1 -type f ! -name SHA256SUMS -print0 \
    | sort -z | xargs -0 shasum -a 256 ) > "$DIST/SHA256SUMS"

echo "checksums: $DIST/SHA256SUMS"
