#lang racket/base

;; Payback backend: device records, payback math, and the online updater,
;; served to the native SwiftUI/WinUI hosts over RVT1. All user-visible
;; numbers are computed here so every platform shows identical values
;; (docs/data-format.md is the contract).

(require json
         racket/file
         racket/path
         rivet/backend
         "domain.rkt"
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
  (define doc (store-doc (the-store)))
  (define today (today-string))
  (define devices (hash-ref doc 'devices))
  (jsexpr->bytes
   (hasheq 'app (hasheq 'version app-version
                         'build app-build
                         'identifier app-identifier
                         'channel (symbol->string app-channel))
           'settings (hash-ref doc 'settings)
           'devices (for/list ([d (in-list devices)]) (device-wire d today))
           'summary (portfolio-summary devices today))))

(define-rpc (add-device [payload Bytes] : Bytes)
  (define body (bytes->jsexpr payload))
  (define today (today-string))
  (define created
    (store-mutate! (the-store)
      (lambda (doc)
        (define settings (hash-ref doc 'settings))
        (define devices (hash-ref doc 'devices))
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

(define auto-check-interval-seconds (* 24 60 60))

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
