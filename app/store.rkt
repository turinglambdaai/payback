#lang racket/base

;; Durable storage for Payback. Since data format v2 (docs/sync-design.md)
;; this module owns the *local* document only — machine identity, the sync
;; folder choice and the per-machine settings that never sync. The synced
;; device ledger lives in per-machine shard files managed by sync.rkt; the
;; v1 single-file `payback.json` is a read-only migration source there.
;;
;; Deliberately free of Rivet dependencies so the persistence layer is
;; unit-testable headlessly. Records use string keys throughout — the
;; in-memory shape is exactly the wire shape.
;;
;; Local document layout (docs/data-format.md):
;;   {"version": 1,
;;    "machine": {"id": "m-<8hex>", "name": "..."},
;;    "syncDir": null,
;;    "devices": [],
;;    "settings": {...local-only settings...}}

(require json
         racket/file
         racket/string)

(provide (struct-out store)
         make-store
         store-doc
         store-mutate!
         write-doc!
         default-settings
         default-machine
         plausible-machine?
         plausible-device?)

(struct store (path sema) #:transparent)

(define local-document-version 1)

;; Local-only settings: the sync whitelist (currency, licenseKey) is merged
;; from shard claims by sync.rkt and never persisted here. `currency` stays
;; as the fallback default for a fresh install with no claims anywhere.
(define (default-settings)
  (hasheq 'currency "CNY"
          'updateAutoCheck #t
          'updateBaseUrl 'null
          'lastUpdateCheckAt 'null
          'rolloutBucket 'null))

(define (default-machine)
  'null)

(define (plausible-machine? value)
  (and (hash? value)
       (string? (hash-ref value 'id #f))
       (string? (hash-ref value 'name #f))))

(define (default-doc)
  (hasheq 'version local-document-version
          'machine (default-machine)
          'syncDir 'null
          'devices '()
          'settings (default-settings)))

(define (complete-doc doc)
  ;; merge in keys added by later versions so an old file keeps working
  (define defaults (default-doc))
  (hasheq 'version (hash-ref doc 'version (hash-ref defaults 'version))
          'machine (hash-ref doc 'machine (default-machine))
          'syncDir (hash-ref doc 'syncDir 'null)
          'devices (hash-ref doc 'devices '())
          'settings
          (for/fold ([settings (hash-ref defaults 'settings)])
                    ([key (in-list (hash-keys (hash-ref doc 'settings (hasheq))))])
            (hash-set settings key (hash-ref (hash-ref doc 'settings) key)))))

;; Structural sanity on the parsed document. A hand-edited file whose JSON
;; is valid but whose shape is wrong (e.g. devices: 5) must take the corrupt
;; path instead of raising inside every load (Taskly 0.6.1 lesson: never
;; crash on data).
(define (plausible-device? device)
  (and (hash? device)
       (string? (hash-ref device 'id #f))
       (string? (hash-ref device 'name #f))
       (exact-integer? (hash-ref device 'priceMinor #f))
       (string? (hash-ref device 'purchaseDate #f))))

(define (plausible-doc? value)
  (and (hash? value)
       (hash? (hash-ref value 'settings (hasheq)))
       (let ([machine (hash-ref value 'machine 'null)])
         (or (eq? machine 'null) (plausible-machine? machine)))
       (let ([sync-dir (hash-ref value 'syncDir 'null)])
         (or (eq? sync-dir 'null) (string? sync-dir)))
       (and (list? (hash-ref value 'devices '()))
            (andmap plausible-device? (hash-ref value 'devices)))))

(define (make-store path)
  (store (path->complete-path path) (make-semaphore 1)))

(define (read-doc path)
  (cond
    [(not (file-exists? path)) (default-doc)]
    [else
     (define value
       (with-handlers ([exn:fail? (lambda (_) #f)])
         (call-with-input-file path read-json #:mode 'text)))
     (cond
       [(plausible-doc? value) (complete-doc value)]
       ;; a corrupt or shape-broken file must never brick the app: preserve
       ;; it for manual recovery and start clean
       [else
        (define backup
          (path-replace-extension
           path
           (format ".corrupt-~a.json" (current-seconds))))
        (with-handlers ([exn:fail? (lambda (_) (void))])
          (rename-file-or-directory path backup #f))
        (default-doc)])]))

(define (write-doc! path doc)
  (make-parent-directory* path)
  (call-with-atomic-output-file path
    (lambda (out temporary-path)
      (void temporary-path)
      (write-json doc out)
      (newline out))))

(define (store-doc a-store)
  (call-with-semaphore (store-sema a-store)
    (lambda () (read-doc (store-path a-store)))))

;; atomically read-modify-write; `f` receives the current document and
;; returns (values new-doc result); the result is passed through. When `f`
;; returns the document object unchanged (eq?), no write happens — reads
;; that only observe (or only ack nothing) stay reads.
(define (store-mutate! a-store f)
  (call-with-semaphore (store-sema a-store)
    (lambda ()
      (define doc (read-doc (store-path a-store)))
      (define-values (new-doc result) (f doc))
      (unless (eq? new-doc doc)
        (write-doc! (store-path a-store) new-doc))
      result)))
