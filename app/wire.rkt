#lang racket/base

;; Wire validation for Payback RPC payloads. Input JSON is converted and
;; normalized into canonical device/settings records before it reaches the
;; store; violations raise with messages the native hosts surface verbatim.
;;
;; Money amounts are integer minor units. `willingPerDayMinor` (心理价位：
;; 用户愿意为这台设备每天支付的钱) drives the payback progress feature and
;; stays optional everywhere.

(require racket/date
         racket/list
         racket/format
         racket/port
         racket/string
         "domain.rkt")

(provide new-device-id
         now-timestamp
         canonical-currency?
         validate-new-device
         validate-device-update
         validate-settings-patch)

(define categories
  '("computer" "phone" "tablet" "audio" "camera" "gaming" "appliance" "accessory" "other"))

(define max-price-minor 1000000000)     ; 10,000,000.00
(define max-willing-minor 100000000)    ;   1,000,000.00 per day
(define max-name-chars 100)
(define max-notes-chars 2000)
(define max-icon-chars 16)

;; randomness without pulling racket/random (whose foreign-library load
;; is not bundled in the packaged Racket framework on macOS)
(define (crypto-random-bytes byte-count)
  (case (system-type 'os)
    [(windows)
     (define rand (dynamic-require 'racket/random 'crypto-random-bytes))
     (rand byte-count)]
    [else
     (define buffer (make-bytes byte-count))
     (call-with-input-file "/dev/urandom"
       (lambda (in) (read-bytes! buffer in))
       #:mode 'binary)
     buffer]))

(define (random-hex-string byte-count)
  (string-append*
   (for/list ([byte (in-bytes (crypto-random-bytes byte-count))])
     (~r byte #:base 16 #:min-width 2 #:pad-string "0"))))

(define (new-device-id)
  (string-append "d-" (random-hex-string 5)))

(define (now-timestamp)
  (define d (seconds->date (current-seconds)))
  (format "~a-~a-~a ~a:~a:~a"
          (date-year d)
          (~r (date-month d) #:min-width 2 #:pad-string "0")
          (~r (date-day d) #:min-width 2 #:pad-string "0")
          (~r (date-hour d) #:min-width 2 #:pad-string "0")
          (~r (date-minute d) #:min-width 2 #:pad-string "0")
          (~r (date-second d) #:min-width 2 #:pad-string "0")))

(define (canonical-currency? s)
  (and (string? s) (regexp-match? #px"^[A-Z]{3}$" s)))

(define (field jsexpr key [default 'absent])
  (define v (hash-ref jsexpr key default))
  (if (eq? v 'null) 'absent v))

(define (require-string who key value limit)
  (unless (string? value)
    (error who "field '~a' must be a string" key))
  (unless (<= (string-length value) limit)
    (error who "field '~a' exceeds ~a characters" key limit))
  value)

;; ---------- devices ----------

;; Builds a canonical device record from a wire payload. `id`, `created-at`
;; and `updated-at` are supplied by the caller; client-supplied values of
;; those fields are ignored on create and on update (updated-at is refreshed).
(define (validate-new-device payload settings today who)
  (unless (hash? payload)
    (error who "request body must be a JSON object"))
  (define name
    (string-trim
     (require-string who 'name (field payload 'name) (+ max-name-chars 2))))
  (unless (and (string? name) (<= 1 (string-length name) max-name-chars))
    (error who "field 'name' must be 1-~a characters" max-name-chars))
  (define icon
    (let ([raw (field payload 'icon)])
      (cond
        [(eq? raw 'absent) "📦"]
        [(and (string? raw) (<= 1 (string-length raw) max-icon-chars)) raw]
        [else (error who "field 'icon' must be 1-~a characters" max-icon-chars)])))
  (define category
    (let ([raw (field payload 'category "other")])
      (unless (member raw categories)
        (error who "field 'category' must be one of: ~a" (string-join categories ", ")))
      raw))
  (define price (field payload 'priceMinor))
  (unless (and (exact-integer? price) (<= 1 price max-price-minor))
    (error who "field 'priceMinor' must be an integer between 1 and ~a" max-price-minor))
  (define currency
    (let ([raw (field payload 'currency (hash-ref settings 'currency))])
      (unless (canonical-currency? raw)
        (error who "field 'currency' must be a 3-letter uppercase ISO 4217 code"))
      raw))
  (define purchase-date (field payload 'purchaseDate))
  (unless (valid-date-range? purchase-date today)
    (error who "field 'purchaseDate' must be a date between ~a and today (~a), formatted yyyy-MM-dd"
           min-date-string today))
  (define willing (field payload 'willingPerDayMinor))
  (unless (or (eq? willing 'absent)
              (and (exact-integer? willing) (<= 1 willing max-willing-minor)))
    (error who "field 'willingPerDayMinor' must be a positive integer up to ~a" max-willing-minor))
  (define notes
    (let ([raw (field payload 'notes "")])
      (unless (string? raw) (error who "field 'notes' must be a string"))
      (unless (<= (string-length raw) max-notes-chars)
        (error who "field 'notes' exceeds ~a characters" max-notes-chars))
      raw))
  (hasheq 'name name
          'icon icon
          'category category
          'priceMinor price
          'currency currency
          'purchaseDate purchase-date
          'willingPerDayMinor (if (eq? willing 'absent) 'null willing)
          'notes notes))

;; Returns the validated full record given the existing record for `id`.
(define (validate-device-update payload existing today who)
  (unless (hash? payload)
    (error who "request body must be a JSON object"))
  (define id (field payload 'id))
  (unless (and (string? id) (string=? id (hash-ref existing 'id)))
    (error who "field 'id' must identify an existing device"))
  (define checked
    (validate-new-device payload
                         (hasheq 'currency (hash-ref existing 'currency))
                         today
                         who))
  (for/fold ([record existing])
            ([key (in-list '(name icon category priceMinor currency
                             purchaseDate willingPerDayMinor notes))])
    (hash-set record key (hash-ref checked key))))

;; ---------- settings ----------

;; Only app-owned keys are patchable here; rolloutBucket and
;; lastUpdateCheckAt are maintained internally by the updater.
(define (validate-settings-patch payload who)
  (unless (hash? payload)
    (error who "request body must be a JSON object"))
  (for/fold ([patch (hasheq)])
            ([key (in-list '(currency updateAutoCheck updateBaseUrl))])
    (define raw (hash-ref payload key 'skip))
    (cond
      [(eq? raw 'skip) patch]
      ;; only updateBaseUrl may be set back to null (reset to default URL)
      [(eq? raw 'null)
       (if (eq? key 'updateBaseUrl)
           (hash-set patch key 'null)
           (error who "field '~a' cannot be null" key))]
      [(and (eq? key 'currency) (canonical-currency? raw))
       (hash-set patch key raw)]
      [(and (eq? key 'updateAutoCheck) (boolean? raw))
       (hash-set patch key raw)]
      [(and (eq? key 'updateBaseUrl)
            (string? raw)
            (or (string=? raw "") (regexp-match? #px"^https://" raw)))
       (hash-set patch key raw)]
      [else (error who "field '~a' has an invalid value" key)])))
