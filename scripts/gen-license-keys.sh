#!/bin/sh
# Generate the Ed25519 license-signing keypair for Payback.
#
# Separate from the update keypair on purpose: one lock, one job
# (COMMERCIAL-CHECKLIST 红线). Same generation path as gen-update-keys.sh —
# Racket's crypto package, because macOS LibreSSL lacks Ed25519 — which also
# proves the DER round-trips through rivet's readers.
#
# Usage: scripts/gen-license-keys.sh [output-dir]
#
# The private key issues customer license tokens (scripts/issue-license.rkt).
# It never enters the repository: keys/ is gitignored except for the public
# half. Back the private key up in Sync/Keys (encrypted), and rotate via the
# key_id claim: ship a build that trusts the next public key before issuing
# exclusively with it.

set -eu

OUT_DIR="${1:-keys}"
mkdir -p "$OUT_DIR"

if [ -f "$OUT_DIR/license-ed25519-private.der" ]; then
  echo "gen-license-keys: $OUT_DIR/license-ed25519-private.der already exists" >&2
  exit 1
fi

SCRIPT="$(mktemp "${TMPDIR:-/tmp}/payback-gen-license-keys-XXXXXX.rkt")"
trap 'rm -f "$SCRIPT"' EXIT

cat > "$SCRIPT" <<'EOF'
#lang racket/base
(require crypto
         crypto/all
         rivet/distribution
         net/base64
         racket/file)
(use-all-factories!)
(define out-dir (vector-ref (current-command-line-arguments) 0))
(define priv (generate-private-key 'eddsa '((curve ed25519))))
(define priv-der (pk-key->datum priv 'OneAsymmetricKey))
;; rkt-public is always the true vk regardless of how the private datum
;; ordered (vk sk) — never pick datum elements by position
(define pub (datum->pk-key (pk-key->datum priv 'rkt-public) 'rkt-public))
(define pub-der (pk-key->datum pub 'SubjectPublicKeyInfo))
(define priv-path (build-path out-dir "license-ed25519-private.der"))
(define pub-path (build-path out-dir "license-ed25519-public.der"))
(call-with-output-file priv-path (lambda (o) (write-bytes priv-der o))
  #:exists 'truncate/replace)
(call-with-output-file pub-path (lambda (o) (write-bytes pub-der o))
  #:exists 'truncate/replace)
(void (read-ed25519-private-key priv-path))
(void (read-ed25519-public-key pub-path))
(printf "private: ~a\npublic DER (base64, embed in app/version.rkt):\n  ~a\n"
        priv-path (bytes->base64-string pub-der))
EOF

racket "$SCRIPT" "$OUT_DIR"
