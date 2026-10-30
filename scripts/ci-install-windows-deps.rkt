#lang racket/base
;; Windows hosted runners can fail loading the Windows ROOT certificate store
;; (X509*->C receives #f). Install dependencies using an explicit, trusted
;; Mozilla CA bundle without disabling TLS or hostname verification.
;; Configure the trust source in the SAME Racket process as pkg downloads.
(require openssl
         pkg
         racket/file)

(define bundle (getenv "PAYBACK_CI_CA_BUNDLE"))
(unless (and bundle
             (file-exists? bundle)
             (regexp-match? #rx#"-----BEGIN CERTIFICATE-----"
                            (file->bytes bundle)))
  (error 'ci-install-windows-deps
         "trusted Mozilla PEM CA bundle is missing or invalid"))

(ssl-default-verify-sources (list bundle))

(define rivet-pin (getenv "RIVET_PIN"))
(unless (and rivet-pin (not (equal? rivet-pin "")))
  (error 'ci-install-windows-deps "RIVET_PIN must be set"))
(define rivet-url (format "https://github.com/turlamai/rivet.git#~a" rivet-pin))

(pkg-install-command #:auto #t #:no-docs #t rivet-url)
