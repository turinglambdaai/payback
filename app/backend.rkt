#lang racket/base

;; Payback backend: device records, payback math, and the online updater,
;; served to the native SwiftUI/WinUI/GTK hosts over RVT1. All user-visible
;; numbers are computed here so every platform shows identical values
;; (docs/data-format.md is the contract).
;;
;; Persistence is data format v2 (docs/sync-design.md): RPCs read the
;; merged view of the per-machine shard files in the active directory, and
;; every mutation claims the changed record for this machine and rewrites
;; only our own shard. The local document (local.json) holds machine
;; identity, the sync folder choice and the settings that never sync.

(require json
         racket/file
         racket/path
         racket/string
         rivet/backend
         "domain.rkt"
         "store.rkt"
         "sync.rkt"
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

;; parameterized so headless tests can point RPCs at a scratch directory;
;; always the *default* (local) directory — the sync folder, if any, is a
;; separate location and never holds the local document or update downloads
(define current-app-data-dir (make-parameter (compute-app-data-dir)))

;; lazy singletons; `start` runs before any RPC
(define local-store #f)
(define (the-local-store)
  (unless local-store
    (set! local-store
          (make-store (build-path (current-app-data-dir) "local.json"))))
  local-store)

(define payback-sync #f)
(define (the-sync)
  (unless payback-sync
    (set! payback-sync (make-sync-engine (the-local-store)))
    (sync-init! payback-sync))
  payback-sync)

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
  (define outcome
    (sync-mutate! (the-sync)
      (lambda (doc)
        (define settings (hash-ref doc 'settings))
        (define devices-list (hash-ref doc 'devices))
        (define seen0 (hash-ref settings 'seenMilestones (hasheq)))
        (define (ack-device seen d)
          ;; mark milestones achieved since the last load-all, then record
          ;; them as seen (acked-on-read celebration flags); the seen map
          ;; is per-machine and lives in the local document, never in a
          ;; shard
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
        (define-values (marked seen^)
          (for/fold ([devices '()] [seen seen0])
                    ([d (in-list devices-list)])
            (define-values (d^ seen^) (ack-device seen d))
            (values (append devices (list d^)) seen^)))
        (cond
          ;; ack-on-read only writes when something new was observed: plain
          ;; loads (and every host reload after add/update/delete) stay
          ;; reads. When acks did change, the local document is updated
          ;; inside the engine lock and the cached view settings rebuilt.
          [(eq? seen^ seen0) (values #f (list marked #f seen^))]
          [else
           (store-mutate! (the-local-store)
             (lambda (ldoc)
               (values (hash-set ldoc 'settings
                                 (hash-set (hash-ref ldoc 'settings)
                                           'seenMilestones seen^))
                       (void))))
           (values (list 'local-settings) (list marked #t seen^))]))))
  (define marked-devices (list-ref outcome 0))
  (define seen-changed? (list-ref outcome 1))
  (define new-seen (list-ref outcome 2))
  ;; devices already carry their computed block (with celebration flags)
  (jsexpr->bytes
   (hasheq 'app (hasheq 'version app-version
                         'build app-build
                         'identifier app-identifier
                         'channel (symbol->string app-channel))
           'settings (hash-ref (sync-view (the-sync)) 'settings)
           'devices marked-devices
           'summary (portfolio-summary/computed marked-devices))))

;; once-per-day digest payload for the native "回本快报" notification
(define-rpc (daily-digest : Bytes)
  (define today (today-string))
  (define payload
    (sync-mutate! (the-sync)
      (lambda (doc)
        (define settings (hash-ref doc 'settings))
        (define last-digest (hash-ref settings 'lastDigestAt ""))
        (cond
          [(and (string? last-digest) (string=? last-digest today))
           (values #f (hasheq 'status "already"))]
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
           (define result
             (hasheq 'status "ok"
                     ;; null when the portfolio mixes currencies — hosts
                     ;; render the per-currency list instead (#23)
                     'earnedTotalMinor (hash-ref summary 'earnedTotalMinor)
                     'earnedPerCurrency
                     (for/list ([g (in-list (hash-ref summary 'currencyGroups))])
                       (hasheq 'currency (hash-ref g 'currency)
                               'earnedTotalMinor
                               (hash-ref g 'earnedTotalMinor)))
                     'deviceCount (hash-ref summary 'deviceCount)
                     'bestDeviceName (if best (hash-ref best 'name) "")
                     ;; the best device's own currency: hosts must not
                     ;; format a foreign amount under the ledger symbol
                     'bestDeviceCurrency
                     (if best (hash-ref best 'currency) 'null)
                     'bestDeviceCostPerDayMinor
                     (if best
                         (hash-ref (hash-ref best 'computed) 'costPerDayMinor)
                         0.0)))
           ;; the digest stamp is local state: each machine gets its own
           ;; once-a-day 快报; written under the engine lock so the cached
           ;; view settings pick it up before the next read
           (store-mutate! (the-local-store)
             (lambda (ldoc)
               (values (hash-set ldoc 'settings
                                 (hash-set (hash-ref ldoc 'settings)
                                           'lastDigestAt today))
                       (void))))
           (values (list 'local-settings) result)]))))
  (jsexpr->bytes payload))

(define-rpc (add-device [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define today (today-string))
  (define created
    (sync-mutate! (the-sync)
      (lambda (doc)
        (define settings (hash-ref doc 'settings))
        (define devices (hash-ref doc 'devices))
        (define record
          (validate-new-device body settings today 'add-device))
        (define now (now-timestamp))
        (define me (sync-machine-id (the-sync)))
        (define full
          (hash-set* record
                     'id (insert-id (sync-known-ids (the-sync)))
                     'createdAt now
                     'updatedAt now
                     'editedBy me
                     'deletedAt 'null))
        (values (list 'device full) full))))
  (jsexpr->bytes (device-wire created today)))

(define-rpc (update-device [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define today (today-string))
  (define updated
    (sync-mutate! (the-sync)
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
        ;; editing (or deleting) a device another machine created moves it
        ;; into our claim set — that is how the LWW merge handles takeover
        (define full
          (hash-set* record
                     'updatedAt (now-timestamp)
                     'editedBy (sync-machine-id (the-sync))
                     'deletedAt 'null))
        (values (list 'device full) full))))
  (jsexpr->bytes (device-wire updated today)))

(define-rpc (delete-device [id String] : Void)
  ;; deletion is a tombstone, never a real delete: a hard delete would let
  ;; the record resurrect from other machines' shards on the next merge
  (sync-mutate! (the-sync)
    (lambda (doc)
      (define existing
        (or (for/first ([d (in-list (hash-ref doc 'devices))]
                        #:when (string=? (hash-ref d 'id) id))
              d)
            (error 'delete-device "unknown device: ~a" id)))
      (define tombstone
        (hash-set* existing
                   'updatedAt (now-timestamp)
                   'editedBy (sync-machine-id (the-sync))
                   'deletedAt (now-timestamp)))
      (values (list 'device tombstone) (void)))))

;; ---------- settings ----------

;; currency is a synced settings claim; the rest of the patch is
;; per-machine local state (docs/sync-design.md whitelist)
(define-rpc (save-settings [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define patch (validate-settings-patch body 'save-settings))
  (when (hash-has-key? patch 'currency)
    (sync-claim-setting! (the-sync) 'currency (hash-ref patch 'currency)))
  (define local-patch
    (for/hasheq ([(key value) (in-hash patch)]
                 #:unless (eq? key 'currency))
      (values key value)))
  (unless (hash-empty? local-patch)
    (store-mutate! (the-local-store)
      (lambda (ldoc)
        (define merged
          (for/fold ([settings (hash-ref ldoc 'settings)])
                    ([key (in-list (hash-keys local-patch))])
            (hash-set settings key (hash-ref local-patch key))))
        (values (hash-set ldoc 'settings merged) (void)))))
  (jsexpr->bytes (hash-ref (sync-view (the-sync)) 'settings)))

;; ---------- sync ----------

;; Cheap poll entry point for the hosts' refresh timer: stats the shard
;; files and re-merges only when the active directory changed externally.
(define-rpc (sync-state : Bytes)
  (sync-refresh-if-changed! (the-sync))
  (jsexpr->bytes (sync-state-payload (the-sync))))

(define-rpc (configure-sync [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define dir (hash-ref body 'dir 'null))
  (jsexpr->bytes (sync-configure! (the-sync) dir)))

(define-rpc (rename-machine [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define name
    (let ([v (hash-ref body 'name 'null)])
      (unless (string? v)
        (error 'rename-machine "field 'name' must be a string"))
      v))
  (jsexpr->bytes (sync-rename-machine! (the-sync) name)))

;; ---------- online updates ----------

;; Family baseline (taskly): silent auto-checks fire at most once every
;; 4 hours; a forced 「检查更新」 bypasses the throttle. Update state is
;; per-machine (never synced) and downloads land in the local directory,
;; never in the sync folder.
(define auto-check-interval-seconds (* 4 60 60))

(define-rpc (check-updates [force Bool] : Bytes)
  (define settings
    (hash-ref (store-doc (the-local-store)) 'settings))
  (define last (hash-ref settings 'lastUpdateCheckAt 'null))
  (define throttled
    (and (not force)
         (exact-integer? last)
         (< (- (current-seconds) last) auto-check-interval-seconds)))
  (if throttled
      (jsexpr->bytes (hasheq 'status "throttled"))
      (jsexpr->bytes (perform-check! (the-local-store) settings))))

(define-rpc (start-download : Void)
  (define settings
    (hash-ref (store-doc (the-local-store)) 'settings))
  (start-download! (the-local-store) settings (current-app-data-dir))
  (void))

(define-rpc (update-state : Bytes)
  (jsexpr->bytes (update-state-snapshot)))

;; ---------- entry ----------

(define (start in-fd out-fd)
  (the-sync)
  (serve-fds in-fd out-fd))
