#!/bin/sh
# Generate the Ed25519 update-signing keypair for Payback.
#
# macOS system LibreSSL lacks Ed25519, so generation goes through Racket's
# crypto package — the same library the updater uses to verify, which also
# proves the DER round-trips through rivet's own readers.
#
# Usage: scripts/gen-update-keys.sh [output-dir]
#
# The private key signs release manifests (raco rivet release, via
# RIVET_UPDATE_PRIVATE_KEY). It never enters the repository: keys/ is
# gitignored except for the public half. Back the private key up outside
# the checkout (encrypted sync + CI secret), and rotate via key_id as
# described in docs/updates.md.

set -eu

OUT_DIR="${1:-keys}"
mkdir -p "$OUT_DIR"

if [ -f "$OUT_DIR/update-ed25519-private.der" ]; then
  echo "gen-update-keys: $OUT_DIR/update-ed25519-private.der already exists" >&2
  exit 1
fi

SCRIPT="$(mktemp /tmp/payback-gen-keys-XXXXXX.rkt)"
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
;; Derive the public half through the library instead of picking datum
;; elements: the rkt-private datum orders (vk sk) for DER-imported keys but
;; (sk vk) for freshly generated ones. rkt-public is always the true vk.
(define pub (datum->pk-key (pk-key->datum priv 'rkt-public) 'rkt-public))
(define pub-der (pk-key->datum pub 'SubjectPublicKeyInfo))
(define priv-path (build-path out-dir "update-ed25519-private.der"))
(define pub-path (build-path out-dir "update-ed25519-public.der"))
(call-with-output-file priv-path (lambda (o) (write-bytes priv-der o))
  #:exists 'truncate/replace)
(call-with-output-file pub-path (lambda (o) (write-bytes pub-der o))
  #:exists 'truncate/replace)
;; prove rivet's release tooling reads both files back
(void (read-ed25519-private-key priv-path))
(void (read-ed25519-public-key pub-path))
(printf "private: ~a\npublic DER (base64, embed in app/version.rkt):\n  ~a\n"
        priv-path (bytes->base64-string pub-der))
EOF

racket "$SCRIPT" "$OUT_DIR"
