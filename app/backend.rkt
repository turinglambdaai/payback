#lang racket/base

;; Payback backend: device records, payback math, and the online updater,
;; served to the native SwiftUI/WinUI hosts over RVT1. All user-visible
;; numbers are computed here so every platform shows identical values
;; (docs/data-format.md is the contract).

(require json
         racket/file
         racket/path
         racket/string
         rivet/backend
         "domain.rkt"
         "license.rkt"
         "store.rkt"
         "updater.rkt"
         "version.rkt"
         "wire.rkt")

(provide start
         current-app-data-dir)

;; ---------- locations ----------

(define (compute-app-data-dir)
  (case (system-type 'os)
    [(macosx)
     (build-path (find-system-path 'home-dir)
                 "Library" "Application Support" "Payback")]
    [(windows)
     (define appdata (getenv "APPDATA"))
     (if (and appdata (not (string=? appdata "")))
         (build-path (string->path appdata) "Payback")
         (build-path (find-system-path 'home-dir)
                     "AppData" "Roaming" "Payback"))]
    [else
     (define xdg (getenv "XDG_DATA_HOME"))
     (if (and xdg (not (string=? xdg "")))
         (build-path (string->path xdg) "payback")
         (build-path (find-system-path 'home-dir) ".local" "share" "payback"))]))

;; parameterized so headless tests can point RPCs at a scratch directory
(define current-app-data-dir (make-parameter (compute-app-data-dir)))

;; lazy singleton; `start` runs before any RPC
(define payback-store #f)
(define (the-store)
  (unless payback-store
    (set! payback-store
          (make-store (build-path (current-app-data-dir) "payback.json"))))
  payback-store)

;; ---------- JSON helpers ----------

(define (jsexpr->bytes j)
  (string->bytes/utf-8 (jsexpr->string j)))

(define (bytes->jsexpr b)
  (define value (read-json (open-input-bytes b)))
  (unless (hash? value)
    (error 'payback "request body must be a JSON object"))
  value)

(define (device-wire device today)
  (hash-set device 'computed (device-computed device today)))

;; ---------- devices ----------

(define (insert-id existing)
  (define id (new-device-id))
  (if (member id existing) (insert-id existing) id))

(define-rpc (load-all : Bytes)
  (define today (today-string))
  (define result
    (store-mutate! (the-store)
      (lambda (doc)
        (define settings (hash-ref doc 'settings))
        (define devices-list (hash-ref doc 'devices))
        (define seen0 (hash-ref settings 'seenMilestones (hasheq)))
        (define (ack-device seen d)
          ;; mark milestones achieved since the last load-all, then record
          ;; them as seen (acked-on-read celebration flags)
          (define computed (device-computed d today))
          (define previously
            (hash-ref seen
                      (string->symbol (hash-ref d 'id)) '()))
          (define new-keys
            (for/list ([m (in-list (hash-ref computed 'milestones))]
                       #:when (and (hash-ref m 'achieved)
                                   (not (member (hash-ref m 'key) previously))))
              (hash-ref m 'key)))
          (define marked
            (hash-set computed 'milestones
                      (for/list ([m (in-list (hash-ref computed 'milestones))])
                        (hash-set m 'new (pair? (member (hash-ref m 'key)
                                                        new-keys))))))
          (define next-seen
            (if (null? new-keys)
                seen
                (hash-set seen (string->symbol (hash-ref d 'id))
                          (append previously new-keys))))
          (values (hash-set d 'computed marked) next-seen))
        (define-values (marked-devices new-seen)
          (for/fold ([devices '()] [seen seen0])
                    ([d (in-list devices-list)])
            (define-values (d^ seen^) (ack-device seen d))
            (values (append devices (list d^)) seen^)))
        ;; ack-on-read only writes when something new was observed: plain
        ;; loads (and every host reload after add/update/delete) stay reads
        (if (eq? new-seen seen0)
            (values doc marked-devices)
            (values (hash-set doc 'settings
                              (hash-set settings 'seenMilestones new-seen))
                    marked-devices)))))
  (define devices result)
  ;; devices already carry their computed block (with celebration flags)
  (jsexpr->bytes
   (hasheq 'app (hasheq 'version app-version
                         'build app-build
                         'identifier app-identifier
                         'channel (symbol->string app-channel))
           'settings (hash-ref (store-doc (the-store)) 'settings)
           'devices devices
           'summary (portfolio-summary/computed devices))))

;; once-per-day digest payload for the native "回本快报" notification
(define-rpc (daily-digest : Bytes)
  (define today (today-string))
  (jsexpr->bytes
   (store-mutate! (the-store)
     (lambda (doc)
       (define settings (hash-ref doc 'settings))
       (define last-digest (hash-ref settings 'lastDigestAt ""))
       (cond
         [(and (string? last-digest) (string=? last-digest today))
          (values doc (hasheq 'status "already"))]
         [else
          (define marked
            (for/list ([d (in-list (hash-ref doc 'devices))])
              (hash-set d 'computed (device-computed d today))))
          (define summary (portfolio-summary/computed marked))
          (define best
            (and (pair? marked)
                 (for/first ([d (in-list marked)]
                             #:when (string=? (hash-ref d 'id)
                                              (hash-ref summary 'bestDeviceId)))
                   d)))
          (define payload
            (hasheq 'status "ok"
                    'earnedTotalMinor (hash-ref summary 'earnedTotalMinor)
                    'deviceCount (hash-ref summary 'deviceCount)
                    'bestDeviceName (if best (hash-ref best 'name) "")
                    'bestDeviceCostPerDayMinor
                    (if best
                        (hash-ref (hash-ref best 'computed) 'costPerDayMinor)
                        0.0)))
          (values (hash-set doc 'settings
                            (hash-set settings 'lastDigestAt today))
                  payload)])))))

(define-rpc (add-device [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define today (today-string))
  (define created
    (store-mutate! (the-store)
      (lambda (doc)
        (define settings (hash-ref doc 'settings))
        (define devices (hash-ref doc 'devices))
        ;; free-tier wall (PRICING.md): 10 devices, a valid Pro token lifts it
        (unless (license-allows-more? (hash-ref settings 'licenseKey #f)
                                      (length devices))
          (error 'add-device
                 "the free version holds up to ~a devices — activate Payback Pro for unlimited"
                 free-device-limit))
        (define record
          (validate-new-device body settings today 'add-device))
        (define now (now-timestamp))
        (define full
          (hash-set* record
                     'id (insert-id devices)
                     'createdAt now
                     'updatedAt now))
        (values (hash-set doc 'devices (append devices (list full)))
                full))))
  (jsexpr->bytes (device-wire created today)))

(define-rpc (update-device [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define today (today-string))
  (define updated
    (store-mutate! (the-store)
      (lambda (doc)
        (define devices (hash-ref doc 'devices))
        (define id
          (let ([v (hash-ref body 'id 'null)])
            (if (string? v)
                v
                (error 'update-device "field 'id' must be a string"))))
        (define existing
          (or (for/first ([d (in-list devices)]
                          #:when (string=? (hash-ref d 'id) id))
                d)
              (error 'update-device "unknown device: ~a" id)))
        (define record
          (validate-device-update body existing today 'update-device))
        (define full (hash-set record 'updatedAt (now-timestamp)))
        (values
         (hash-set doc 'devices
                   (for/list ([d (in-list devices)])
                     (if (string=? (hash-ref d 'id) id) full d)))
         full))))
  (jsexpr->bytes (device-wire updated today)))

(define-rpc (delete-device [id String] : Void)
  (store-mutate! (the-store)
    (lambda (doc)
      (define devices (hash-ref doc 'devices))
      (unless (for/first ([d (in-list devices)]
                          #:when (string=? (hash-ref d 'id) id))
                d)
        (error 'delete-device "unknown device: ~a" id))
      (values
       (hash-set doc 'devices
                 (for/list ([d (in-list devices)]
                            #:unless (string=? (hash-ref d 'id) id))
                   d))
       (void)))))

;; ---------- license ----------

;; Activate Payback Pro: verify the pasted token offline, store it in
;; settings. Re-activation with a new token replaces the old one.
(define-rpc (activate-license [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define key
    (let ([v (hash-ref body 'key 'null)])
      (unless (string? v)
        (error 'activate-license "field 'key' must be a string"))
      (string-trim v)))
  (define-values (claims reason)
    (parse-and-verify key))
  (unless claims
    (error 'activate-license "~a" (license-activation-error reason)))
  (jsexpr->bytes
   (store-mutate! (the-store)
     (lambda (doc)
       (define settings^
         (hash-set (hash-ref doc 'settings) 'licenseKey key))
       (values (hash-set doc 'settings settings^)
               (license-state-payload key))))))

;; Current license state; re-verifies the stored token so an expired license
;; downgrades the app to the free tier without any user action.
(define-rpc (license-state : Bytes)
  (jsexpr->bytes
   (license-state-payload
    (hash-ref (hash-ref (store-doc (the-store)) 'settings) 'licenseKey #f))))

;; ---------- settings ----------

(define-rpc (save-settings [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define patch (validate-settings-patch body 'save-settings))
  (jsexpr->bytes
   (store-mutate! (the-store)
     (lambda (doc)
       (define merged
         (for/fold ([settings (hash-ref doc 'settings)])
                   ([key (in-list (hash-keys patch))])
           (hash-set settings key (hash-ref patch key))))
       (values (hash-set doc 'settings merged) merged)))))

;; ---------- online updates ----------

;; Family baseline (taskly): silent auto-checks fire at most once every
;; 4 hours; a forced 「检查更新」 bypasses the throttle.
(define auto-check-interval-seconds (* 4 60 60))

(define-rpc (check-updates [force Bool] : Bytes)
  (define settings (hash-ref (store-doc (the-store)) 'settings))
  (define last (hash-ref settings 'lastUpdateCheckAt 'null))
  (define throttled
    (and (not force)
         (exact-integer? last)
         (< (- (current-seconds) last) auto-check-interval-seconds)))
  (if throttled
      (jsexpr->bytes (hasheq 'status "throttled"))
      (jsexpr->bytes (perform-check! (the-store) settings))))

(define-rpc (start-download : Void)
  (define settings (hash-ref (store-doc (the-store)) 'settings))
  (start-download! (the-store) settings (current-app-data-dir))
  (void))

(define-rpc (update-state : Bytes)
  (jsexpr->bytes (update-state-snapshot)))

;; ---------- entry ----------

(define (start in-fd out-fd)
  (the-store)
  (serve-fds in-fd out-fd))
