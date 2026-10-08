#lang racket/base

;; License token tests: issue → verify round-trip with a throwaway Ed25519
;; keypair, plus every failure path (tamper, wrong product/key-id/type,
;; expiry). Same offline pattern as the updater trust chain tests.

(require crypto
         crypto/all
         rackunit
         net/base64
         rivet/distribution
         racket/list
         racket/string
         "../app/license.rkt")

(use-all-factories!)

;; throwaway keypair for the whole module
(define priv (generate-private-key 'eddsa '((curve ed25519))))
(define pub
  (datum->pk-key (pk-key->datum priv 'rkt-public) 'rkt-public))
(define pub-b64 (bytes->base64-string (pk-key->datum pub 'SubjectPublicKeyInfo)))

(define (verify token #:today [today "2026-10-08"])
  (parameterize ([current-license-public-key-b64 pub-b64]
                 [current-license-key-id "test-license-key"])
    (parse-and-verify token #:today today)))

;; state/wall helpers: these must run under the SAME parameterization or
;; they would verify against the embedded production key
(define (state-for token #:today [today "2026-10-08"])
  (parameterize ([current-license-public-key-b64 pub-b64]
                 [current-license-key-id "test-license-key"])
    (license-state-payload token #:today today)))

(define (wall token count #:today [today "2026-10-08"])
  (parameterize ([current-license-public-key-b64 pub-b64]
                 [current-license-key-id "test-license-key"])
    (license-allows-more? token count #:today today)))

(test-case "perpetual token round-trips"
  (define token
    (parameterize ([current-license-key-id "test-license-key"])
      (issue-pro-token #:private-key priv #:subject "customer@example.com")))
  (check-true (string-prefix? token "PB1."))
  (define-values (claims reason) (verify token))
  (check-false reason)
  (check-equal? (hash-ref claims 'subject) "customer@example.com")
  (check-equal? (hash-ref claims 'type) "pro")
  (check-false (hash-ref claims 'expiry #f))
  ;; licensed-state payload derives the free/pro split
  (define state (state-for token))
  (check-true (hash-ref state 'licensed))
  (check-equal? (hash-ref state 'deviceLimit) 'null))

(test-case "expiring token honors the boundary day inclusively"
  (define (token/exp e)
    (parameterize ([current-license-key-id "test-license-key"])
      (issue-pro-token #:private-key priv #:subject "s" #:expiry e)))
  (define-values (claims reason) (verify (token/exp "2026-10-08")))
  (check-false reason "expiry day itself is still valid")
  (check-equal? (hash-ref claims 'expiry) "2026-10-08")
  (define-values (_ expired) (verify (token/exp "2026-10-07") #:today "2026-10-08"))
  (check-equal? expired "expired")
  ;; an expired token downgrades the state payload instead of erroring
  (define state (state-for (token/exp "2026-10-07")))
  (check-false (hash-ref state 'licensed))
  (check-equal? (hash-ref state 'deviceLimit) free-device-limit)
  (check-equal? (hash-ref state 'reason) "expired"))

(test-case "tampered and foreign tokens fail closed"
  (define token
    (parameterize ([current-license-key-id "test-license-key"])
      (issue-pro-token #:private-key priv #:subject "s")))
  ;; flipping a payload character must break the signature
  (define parts (string-split token "."))
  (define payload-b64 (second parts))
  (define flipped
    (string-append
     (substring payload-b64 0 4)
     (string (if (char=? (string-ref payload-b64 4) #\A) #\B #\A))
     (substring payload-b64 5)))
  (define-values (_v sig-reason)
    (verify (string-join (list "PB1" flipped (third parts)) ".")))
  (check-equal? sig-reason "signature")
  ;; wrong key id (rotation guard), wrong product — both signed by the same
  ;; key so verification passes and the claim checks are what reject
  (define wrong-key-id
    (parameterize ([current-license-key-id "another-key"])
      (issue-pro-token #:private-key priv #:subject "s")))
  (check-equal? (let-values ([(_ r) (verify wrong-key-id)]) r) "key-id")
  (define foreign
    (parameterize ([current-license-key-id "test-license-key"])
      (issue-pro-token #:private-key priv #:subject "s"
                       #:product "other.product")))
  (check-equal? (let-values ([(_ r) (verify foreign)]) r) "product")
  ;; garbage never gets past the parser
  (check-equal? (let-values ([(_ r) (verify "not-a-token")]) r) "malformed")
  (check-equal? (let-values ([(_ r) (verify "")]) r) "malformed"))

(test-case "free-tier wall math"
  (define token
    (parameterize ([current-license-key-id "test-license-key"])
      (issue-pro-token #:private-key priv #:subject "s")))
  (check-true (wall #f 9)
              "under the cap no license is needed")
  (check-false (wall #f free-device-limit)
               "at the cap without a license the wall holds")
  (check-true (wall token free-device-limit)
              "a valid pro token lifts the cap")
  (check-false (wall "PB1.bogus.bogus" free-device-limit)
               "an invalid token does not")
  (check-equal? (license-activation-error "expired")
                "this license expired; a current license is required for Pro")
  (check-equal? (license-activation-error "signature")
                "this license key could not be verified"))
