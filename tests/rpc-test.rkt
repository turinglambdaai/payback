#lang racket/base

;; Full-protocol RPC tests for the payback backend: drive the real RVT1
;; server over pipes exactly the way the native hosts do, against a scratch
;; data directory.

(require rackunit
         json
         rivet/distribution
         racket/file
         racket/list
         racket/path
         racket/string
         rivet/backend
         rivet/protocol
         (file "../app/backend.rkt")
         (file "../app/domain.rkt")
         (file "../app/store.rkt")
         (file "../app/sync.rkt"))

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
 (list "add-device" "check-updates" "configure-sync"
       "daily-digest" "delete-device" "load-all"
       "rename-machine" "save-settings" "start-download" "sync-state"
       "update-device" "update-state"))

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

;; daily digest: first call of a day returns the full payload, second is throttled
(define digest-1 (call-rpc "daily-digest"))
(check-equal? (hash-ref digest-1 'status) "ok")
(check-equal? (hash-ref digest-1 'deviceCount) 1)
(check-equal? (hash-ref digest-1 'bestDeviceName) "Cheapest Earbuds")
(check-true (real? (hash-ref digest-1 'earnedTotalMinor)))
(check-true (> (hash-ref digest-1 'earnedTotalMinor) 0)
            "the earbuds earned money back long ago")
(check-true (real? (hash-ref digest-1 'bestDeviceCostPerDayMinor)))
(define digest-2 (call-rpc "daily-digest"))
(check-equal? (hash-ref digest-2 'status) "already")
;; the digest day is persisted in settings and on disk, so a restart of the
;; app stays throttled for the rest of the day
(check-equal? (hash-ref (hash-ref (call-rpc "load-all") 'settings) 'lastDigestAt)
              (today-string))
(define stored-doc
  (call-with-input-file (build-path tmp-dir "local.json") read-json))
(check-equal? (hash-ref (hash-ref stored-doc 'settings) 'lastDigestAt)
              (today-string))
(check-true (hash-has-key? (hash-ref stored-doc 'settings) 'seenMilestones)
            "milestone acks survive a restart via the local document")

;; ---------- validation caps accept their exact boundary ----------

(define boundary
  (call-rpc "add-device"
            (jsexpr->bytes
             (hasheq 'name "Boundary" 'icon "🎧" 'category "audio"
                     'priceMinor 1000000000          ; cap: 10,000,000.00
                     'currency "CNY"
                     'purchaseDate "2024-01-01"
                     'willingPerDayMinor 100000000   ; cap: 1,000,000.00/day
                     'notes ""))))
(check-equal? (hash-ref boundary 'priceMinor) 1000000000)
(check-equal? (hash-ref boundary 'willingPerDayMinor) 100000000)
(call-rpc "delete-device" (hash-ref boundary 'id))

;; ---------- a populated store over the wire ----------

(define (add-filler-payload n)
  (hasheq 'name (format "Filler ~a" n) 'icon "📻" 'category "audio"
          'priceMinor 50000 'currency "CNY"
          'purchaseDate "2025-01-01"
          'willingPerDayMinor 100
          'notes ""))

;; the store currently holds Cheapest Earbuds; fill past the historical
;; free-tier cap — removal of that gate (#20) must not reintroduce a limit
(define (add-filler n)
  (call-rpc "add-device" (jsexpr->bytes (add-filler-payload n))))
(let loop ([n 2])
  (when (<= n 11)
    (add-filler n)
    (loop (add1 n))))

;; the scratch store landed in the injected directory, not the user's: the
;; local document and our shard both live there (no v1 file on a fresh v2
;; install — payback.json only exists as a migrated backup)
(check-true (file-exists? (build-path tmp-dir "local.json")))
(define machine-id
  (hash-ref (hash-ref (call-rpc "sync-state") 'machine) 'id))
(check-true
 (file-exists?
  (build-path tmp-dir (string-append "devices-" machine-id ".json")))
 "our shard file exists in the local directory")
(check-false (file-exists? (build-path tmp-dir "payback.json")))

;; a foreign-currency device splits the summary into per-currency groups
;; and nulls the combined money scalars; deleting it restores them (#23)
(define usd-id
  (hash-ref
   (call-rpc "add-device"
             (jsexpr->bytes
              (hasheq 'name "US Import" 'icon "📦" 'category "other"
                      'priceMinor 29900 'currency "USD"
                      'purchaseDate "2025-01-01"
                      'willingPerDayMinor 100
                      'notes "")))
   'id))
(define mixed-summary (hash-ref (call-rpc "load-all") 'summary))
(check-equal? (hash-ref mixed-summary 'totalSpentMinor) 'null)
(check-equal? (hash-ref mixed-summary 'avgCostPerDayMinor) 'null)
(check-equal? (hash-ref mixed-summary 'earnedTotalMinor) 'null)
(check-equal? (hash-ref mixed-summary 'deviceCount) 12)
(check-equal?
 (map (λ (g) (hash-ref g 'currency))
      (hash-ref mixed-summary 'currencyGroups))
 '("CNY" "USD"))
(check-equal?
 (hash-ref
  (findf (λ (g) (string=? (hash-ref g 'currency) "USD"))
         (hash-ref mixed-summary 'currencyGroups))
  'totalSpentMinor)
 29900)
(call-rpc "delete-device" usd-id)
(define solo-summary (hash-ref (call-rpc "load-all") 'summary))
(check-true (integer? (hash-ref solo-summary 'totalSpentMinor)))
(check-equal? (length (hash-ref solo-summary 'currencyGroups)) 1)

;; ---------- sync RPCs ----------

(define state0 (call-rpc "sync-state"))
(check-false (hash-ref state0 'enabled))
(check-equal? (hash-ref state0 'dir) (dir-string tmp-dir))
(check-equal? (hash-ref state0 'defaultDir) (dir-string tmp-dir))
(check-true (exact-integer? (hash-ref state0 'revision)))
(check-equal? (hash-ref state0 'ignoredFiles) '())

;; enable sync into a scratch subfolder: claims move, the choice persists
(define sync-dir (build-path tmp-dir "sync-folder"))
(define state-on
  (call-rpc "configure-sync" (jsexpr->bytes (hasheq 'dir (path->string sync-dir)))))
(check-true (hash-ref state-on 'enabled))
(check-equal? (hash-ref state-on 'dir) (path->string sync-dir))
(check-true
 (file-exists?
  (build-path sync-dir (string-append "devices-" machine-id ".json")))
 "our shard moved into the sync folder")
(check-equal?
 (length (hash-ref (call-rpc "load-all") 'devices))
 11
 "devices still visible after the move")
(check-equal?
 (hash-ref (hash-ref (call-rpc "load-all") 'settings) 'currency)
 "USD"
 "currency claim survived the move through the merged view")

;; machine rename round-trips through shard meta and the local document
(define renamed
  (call-rpc "rename-machine" (jsexpr->bytes (hasheq 'name "RPC Test Rig"))))
(check-equal? (hash-ref (hash-ref renamed 'machine) 'name) "RPC Test Rig")
(check-equal?
 (hash-ref (hash-ref (call-rpc "sync-state") 'machine) 'name)
 "RPC Test Rig")

;; back to local-only: the shard comes home for the next launch
(define state-off
  (call-rpc "configure-sync" (jsexpr->bytes (hasheq 'dir 'null))))
(check-false (hash-ref state-off 'enabled))
(check-equal? (hash-ref state-off 'dir) (dir-string tmp-dir))
(check-true
 (file-exists?
  (build-path tmp-dir (string-append "devices-" machine-id ".json")))
 "claims came home")

;; a bad folder is a user-visible error, not a state change
(check-true
 (string-contains?
  (call-rpc/expect-error
   "configure-sync" (jsexpr->bytes (hasheq 'dir "relative/path")))
  "absolute folder path"))
(check-false (hash-ref (call-rpc "sync-state") 'enabled))
