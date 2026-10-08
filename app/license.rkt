#lang racket/base

;; Offline Payback Pro license tokens (COMMERCIAL-CHECKLIST: license 模块).
;; A token is "PB1.<b64url(canonical claims JSON)>.<b64url(signature)>",
;; Ed25519-signed with the license keypair (scripts/gen-license-keys.sh,
;; separate from the update keypair — one lock, one job). Verification is
;; fully offline: no server, no call-home; the token is what the customer
;; pastes into the activation dialog.
;;
;; Claims (canonical JSON, sorted keys): product, subject, type, key_id,
;; and optional expiry "YYYY-MM-DD" (expiry day inclusive). This is an
;; honesty scheme, not DRM: the data file belongs to the user, and a local
;; attacker can always patch the app — same posture as glaze/license.

(require crypto
         crypto/all
         json
         net/base64
         rivet/distribution
         racket/format
         racket/list
         racket/port
         racket/string
         "domain.rkt"
         "version.rkt")

(use-all-factories!)

(provide free-device-limit
         current-license-public-key-b64
         current-license-key-id
         issue-pro-token
         parse-and-verify
         license-state-payload
         license-allows-more?
         license-activation-error)

;; the free tier promise in PRICING.md: up to 10 devices
(define free-device-limit 10)

;; parameterized so tests can drive the RPC server with a throwaway keypair
(define current-license-public-key-b64
  (make-parameter license-public-key-b64))
(define current-license-key-id (make-parameter license-key-id))

;; ---------- canonical JSON (same algorithm as glaze/license) ----------

(define (canonical-json v)
  (string->bytes/utf-8 (json-fragment v)))

(define (json-fragment v)
  (cond
    [(hash? v)
     (string-append
      "{"
      (string-join (for/list ([k (in-list (sort (hash-keys v) string<? #:key symbol->string))])
                     (format "~a:~a" (json-string (symbol->string k))
                             (json-fragment (hash-ref v k))))
                   ",")
      "}")]
    [(list? v) (string-append "[" (string-join (map json-fragment v) ",") "]")]
    [(string? v) (json-string v)]
    [(real? v) (~a v)]
    [(boolean? v) (if v "true" "false")]
    [else (json-string (format "~a" v))]))

(define (json-string s)
  (string-append "\""
                 (string-join (for/list ([c (in-string s)])
                                (case c
                                  [(#\") "\\\""]
                                  [(#\\) "\\\\"]
                                  [(#\newline) "\\n"]
                                  [(#\return) "\\r"]
                                  [(#\tab) "\\t"]
                                  [else (string c)]))
                              "")
                 "\""))

;; ---------- base64url (paste-safe token alphabet) ----------

(define (b64url-encode bytes)
  (define standard
    (string-trim (bytes->string/utf-8 (base64-encode bytes #""))))
  (string-replace
   (string-replace (string-replace standard "=" "") "+" "-")
   "/" "_"))

(define (b64url-decode s)
  (define padded
    (case (modulo (string-length s) 4)
      [(2) (string-append s "==")]
      [(3) (string-append s "=")]
      [else s]))
  (base64-decode
   (string->bytes/utf-8 (string-replace
                         (string-replace padded "-" "+")
                         "_" "/"))))

;; ---------- issuing (vendor side) ----------

;; Returns the PB1 token string for one customer.
(define (issue-pro-token #:private-key private-key
                         #:subject subject
                         #:expiry [expiry #f]
                         #:key-id [key-id (current-license-key-id)]
                         #:product [product app-identifier])
  ;; perpetual licenses carry no expiry field at all, keeping the claims
  ;; schema stable: expiry is present iff the license is time-boxed
  (define claims
    (hasheq 'product product
            'subject subject
            'type "pro"
            'key_id key-id))
  (define claims^
    (if expiry
        (hash-set claims 'expiry expiry)
        claims))
  (define payload (canonical-json claims^))
  (define sig (ed25519-sign private-key payload))
  (string-append "PB1."
                 (b64url-encode payload)
                 "."
                 (b64url-encode sig)))

;; ---------- verification (app side, fully offline) ----------

;; Returns (values claims reason): claims is the claims hash on a valid
;; token, #f otherwise; reason is #f or one of "malformed" "signature"
;; "product" "key-id" "type" "expired".
(define (parse-and-verify token
                          #:today [today (today-string)]
                          #:product [product app-identifier]
                          #:key-id [key-id (current-license-key-id)]
                          #:public-key-b64 [public-key-b64
                                            (current-license-public-key-b64)])
  ;; let/cc, not let/ec, and the escape passes two arguments — never
  ;; (return (values ...)): a single argument position rejects two values
  (let/cc return
    (define (fail reason) (return #f reason))
    (define parts
      (and (string? token) (string-split token ".")))
    (unless (and parts (= (length parts) 3) (string=? (first parts) "PB1"))
      (fail "malformed"))
    (define claims
      (with-handlers ([exn:fail? (lambda (_) #f)])
        (define raw (b64url-decode (second parts)))
        (and raw (with-handlers ([exn:fail? (lambda (_) #f)])
                   (call-with-input-bytes raw read-json)))))
    (unless (hash? claims)
      (fail "malformed"))
    (define sig
      (with-handlers ([exn:fail? (lambda (_) #f)])
        (b64url-decode (third parts))))
    (unless (bytes? sig)
      (fail "malformed"))
    (define public-key
      (with-handlers ([exn:fail? (lambda (_) #f)])
        (datum->pk-key (base64-string->bytes public-key-b64)
                       'SubjectPublicKeyInfo)))
    (unless public-key
      (fail "malformed"))
    ;; note rivet's argument order: (public-key message signature)
    (unless (ed25519-verify public-key (canonical-json claims) sig)
      (fail "signature"))
    (unless (equal? (hash-ref claims 'product #f) product)
      (fail "product"))
    (unless (equal? (hash-ref claims 'key_id #f) key-id)
      (fail "key-id"))
    (unless (equal? (hash-ref claims 'type #f) "pro")
      (fail "type"))
    (define expiry (hash-ref claims 'expiry #f))
    (when (eq? expiry 'null) (set! expiry #f))
    (when (and expiry (not (date-string? expiry)))
      (fail "malformed"))
    (when (and expiry (string<? expiry today))
      (fail "expired"))
    (values claims #f)))

;; friendly user-visible message for an activation failure (hosts surface
;; backend messages verbatim)
(define (license-activation-error reason)
  (case reason
    [("expired")
     (format "this license expired; a current license is required for Pro")]
    [else "this license key could not be verified"]))

;; ---------- state ----------

;; The license-state RPC payload for a stored key ('null when none), and the
;; shared pro-check behind the free-tier wall.
(define (license-state-payload license-key
                               #:today [today (today-string)])
  (if (string? license-key)
      (let-values ([(claims reason)
                    (parse-and-verify license-key #:today today)])
        (if claims
            (hasheq 'licensed #t
                    'type (hash-ref claims 'type)
                    'subject (hash-ref claims 'subject)
                    'expiry (or (hash-ref claims 'expiry #f) 'null)
                    'deviceLimit 'null
                    'reason 'null)
            (hasheq 'licensed #f
                    'type 'null
                    'subject 'null
                    'expiry 'null
                    'deviceLimit free-device-limit
                    'reason reason)))
      (hasheq 'licensed #f
              'type 'null
              'subject 'null
              'expiry 'null
              'deviceLimit free-device-limit
              'reason 'null)))

;; May one more device be added given the stored license key and the current
;; device count?
(define (license-allows-more? license-key current-count
                              #:today [today (today-string)])
  (or (< current-count free-device-limit)
      (let-values ([(claims reason)
                    (parse-and-verify license-key #:today today)])
        (and claims #t))))  ; a valid (unexpired) pro token lifts the cap
