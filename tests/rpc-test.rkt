#lang racket/base

;; Full-protocol RPC tests for the payback backend: drive the real RVT1
;; server over pipes exactly the way the native hosts do, against a scratch
;; data directory.

(require rackunit
         json
         racket/file
         racket/list
         racket/path
         racket/string
         rivet/backend
         rivet/protocol
         (file "../app/backend.rkt"))

;; ---------- server plumbing ----------

(define-values (server-in client-out) (make-pipe))
(define-values (client-in server-out) (make-pipe))

(define tmp-dir
  (path->complete-path
   (make-temporary-file "payback-rpc-test-~a" 'directory)))

(define server-thread
  (parameterize ([current-app-data-dir tmp-dir])
    (thread (lambda () (serve server-in server-out)))))

(define (read-frame/timeout in [seconds 5])
  (define result (make-channel))
  (thread (lambda () (channel-put result (read-frame in))))
  (define value (sync/timeout seconds result))
  (unless value
    (error 'read-frame/timeout "timed out waiting for Rivet frame"))
  value)

(define next-id (box 0))

(define (call-rpc name . args)
  (define id (begin (set-box! next-id (add1 (unbox next-id)))
                    (unbox next-id)))
  (write-frame (frame message:request id (encode-value (list* name args)))
               client-out)
  (define response (read-frame/timeout client-in))
  (check-equal? (frame-id response) id)
  (cond
    [(equal? (frame-type response) message:response)
     ;; Bytes-typed results carry UTF-8 JSON; parse to a jsexpr
     (define value (decode-value (frame-payload response)))
     (if (bytes? value)
         (read-json (open-input-bytes value))
         value)]
    [else
     (error 'call-rpc "request failed: ~a"
            (decode-value (frame-payload response)))]))

;; RPC that is expected to fail: returns the wire error message
(define (call-rpc/expect-error name . args)
  (define id (begin (set-box! next-id (add1 (unbox next-id)))
                    (unbox next-id)))
  (write-frame (frame message:request id (encode-value (list* name args)))
               client-out)
  (define response (read-frame/timeout client-in))
  (check-true (equal? (frame-type response) message:error))
  (decode-value (frame-payload response)))

;; ---------- handshake ----------

(define hello (read-frame/timeout client-in))
(check-equal? (frame-type hello) message:hello)
(check-equal? (decode-value (frame-payload hello))
              (list "rivet" protocol-version))

;; ---------- schema ----------

(define rpc-names
  (for/list ([entry (in-list (rpc-schema))]) (hash-ref entry 'name)))
(check-equal?
 rpc-names
 (list "add-device" "check-updates" "daily-digest" "delete-device" "load-all"
       "save-settings" "start-download" "update-device" "update-state"))

;; ---------- devices over the wire ----------

(define (valid-device-payload #:name [name "MacBook Pro"])
  (hasheq 'name name
          'icon "💻"
          'category "computer"
          'priceMinor 1299900
          'currency "CNY"
          'purchaseDate "2024-06-01"
          'willingPerDayMinor 3000
          'notes ""))

(define created
  (call-rpc "add-device" (jsexpr->bytes (valid-device-payload))))

(check-true (hash-has-key? created 'id))
(check-equal? (hash-ref created 'name) "MacBook Pro")
(check-true (exact-integer? (hash-ref created 'priceMinor)))
(define computed (hash-ref created 'computed))
(check-true (> (hash-ref computed 'daysHeld) 400))
(check-true (real? (hash-ref computed 'costPerDayMinor)))
(check-true (hash-ref computed 'willingSet))
(check-true (hash-ref computed 'paidBack))  ; 300/day over ~480 days >= price
(check-equal? (length (hash-ref computed 'milestones)) 9)

;; load-all reflects the stored device and carries app identity
(define document* (call-rpc "load-all"))
(define rktd (file->value "../rivet.rktd"))
(check-equal? (hash-ref document* 'app)
              (hasheq 'version (hash-ref rktd 'version)
                      'build (hash-ref rktd 'build)
                      'identifier "site.jrtx.payback" 'channel "stable"))
(check-equal? (length (hash-ref document* 'devices)) 1)
(check-equal? (hash-ref (hash-ref document* 'summary) 'deviceCount) 1)
(check-true (hash? (hash-ref document* 'settings)))

;; validation failures come back as wire errors, never crashes
(check-true
 (string-contains?
  (call-rpc/expect-error
   "add-device" (jsexpr->bytes (hasheq 'name "X" 'priceMinor 0
                                       'purchaseDate "2026-01-01")))
  "priceMinor"))

(check-true
 (string-contains?
  (call-rpc/expect-error
   "add-device"
   (jsexpr->bytes (hasheq 'name "X" 'priceMinor 100
                          'purchaseDate "2099-01-01")))
  "purchaseDate"))

;; update renames the device and keeps identity fields server-owned
(define updated
  (call-rpc "update-device"
            (jsexpr->bytes
             (hash-set* (valid-device-payload #:name "MacBook Air")
                        'id (hash-ref created 'id)))))
(check-equal? (hash-ref updated 'name) "MacBook Air")
(check-equal? (hash-ref updated 'id) (hash-ref created 'id))
(check-true (string-contains?
             (call-rpc/expect-error
              "update-device"
              (jsexpr->bytes
               (hash-set (valid-device-payload) 'id "d-missing")))
             "unknown device"))

;; delete removes it
(call-rpc "delete-device" (hash-ref created 'id))
(check-equal? (length (hash-ref (call-rpc "load-all") 'devices)) 0)
(call-rpc/expect-error "delete-device" "d-missing")

;; ---------- milestone celebration flags ----------

(define cheap
  (call-rpc "add-device"
            (jsexpr->bytes
             (hasheq 'name "Cheapest Earbuds" 'icon "🎧" 'category "audio"
                     'priceMinor 100 'currency "CNY"
                     'purchaseDate "2020-01-01"
                     'willingPerDayMinor 10
                     'notes ""))))
;; celebration flags ride on load-all: the first pass flags achievements
;; as new, and the flag is acked on read
(define (find-device id)
  (for/first ([d (in-list (hash-ref (call-rpc "load-all") 'devices))]
              #:when (string=? (hash-ref d 'id) id))
    d))
(define first-pass (find-device (hash-ref cheap 'id)))
(check-true
 (for/or ([m (in-list (hash-ref (hash-ref first-pass 'computed) 'milestones))])
   (and (hash-ref m 'achieved) (hash-ref m 'new #f)))
 "a first-launch achievement is flagged new")
;; a second load-all must not flag the same milestones again (ack on read)
(define second-pass (find-device (hash-ref cheap 'id)))
(check-false
 (for/or ([m (in-list (hash-ref (hash-ref second-pass 'computed) 'milestones))])
   (hash-ref m 'new #f))
 "ack-on-read: milestones stop being new after the first load")

;; ---------- settings ----------

(define merged
  (call-rpc "save-settings" (jsexpr->bytes (hasheq 'currency "USD"))))
(check-equal? (hash-ref merged 'currency) "USD")
(check-equal? (hash-ref (hash-ref (call-rpc "load-all") 'settings) 'currency)
              "USD")
(check-true
 (string-contains?
  (call-rpc/expect-error
   "save-settings" (jsexpr->bytes (hasheq 'currency "cny")))
  "currency"))

;; ---------- updates: never-crash contract ----------

;; point at a dead local endpoint so the check fails fast and offline
(call-rpc "save-settings"
          (jsexpr->bytes (hasheq 'updateBaseUrl "https://127.0.0.1:9/payback")))
(define check
  (call-rpc "check-updates" #t))
(check-not-false
 (member (hash-ref check 'status)
         (list "error" "up-to-date" "available" "throttled"))
 "check-updates always answers with a known status")

;; a download without an available candidate must fail cleanly
(check-not-false
 (string-contains? (call-rpc/expect-error "start-download") "no update"))

(define state (call-rpc "update-state"))
(check-not-false
 (member (hash-ref state 'phase)
         (list "idle" "checking" "downloading" "downloaded" "error")))

;; a failed check is not recorded as "checked": the next launch retries
;; instead of staying silent for a day
(define auto-check (call-rpc "check-updates" #f))
(check-equal? (hash-ref auto-check 'status) "error")

;; daily digest: first call of a day returns data, second is throttled
(define digest-1 (call-rpc "daily-digest"))
(check-not-false (member (hash-ref digest-1 'status) (list "ok" "already")))
(define digest-2 (call-rpc "daily-digest"))
(check-equal? (hash-ref digest-2 'status) "already")

;; the scratch store landed in the injected directory, not the user's
(check-true (file-exists? (build-path tmp-dir "payback.json")))
