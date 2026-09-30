#lang racket/base

;; Durable storage for Payback: one JSON document holding devices and
;; settings, written atomically. Deliberately free of Rivet dependencies so
;; the persistence layer is unit-testable headlessly. Records use string keys
;; throughout — the in-memory shape is exactly the wire shape.
;;
;; Document layout (docs/data-format.md):
;;   {"version": 1, "devices": [...], "settings": {...}}

(require json
         racket/file
         racket/string)

(provide (struct-out store)
         make-store
         store-doc
         store-mutate!
         default-settings)

(struct store (path sema) #:transparent)

(define data-format-version 1)

(define (default-settings)
  (hasheq 'currency "CNY"
          'updateAutoCheck #t
          'updateBaseUrl 'null
          'lastUpdateCheckAt 'null
          'rolloutBucket 'null))

(define (default-doc)
  (hasheq 'version data-format-version
          'devices '()
          'settings (default-settings)))

(define (complete-doc doc)
  ;; merge in keys added by later versions so an old file keeps working
  (define defaults (default-doc))
  (hasheq 'version (hash-ref doc 'version (hash-ref defaults 'version))
          'devices (hash-ref doc 'devices '())
          'settings
          (for/fold ([settings (hash-ref defaults 'settings)])
                    ([key (in-list (hash-keys (hash-ref doc 'settings (hasheq))))])
            (hash-set settings key (hash-ref (hash-ref doc 'settings) key)))))

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
       [(hash? value) (complete-doc value)]
       ;; a corrupt file must never brick the app: preserve it for manual
       ;; recovery and start clean (Taskly 0.6.1 lesson: never crash on data)
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
;; returns (values new-doc result); the result is passed through
(define (store-mutate! a-store f)
  (call-with-semaphore (store-sema a-store)
    (lambda ()
      (define doc (read-doc (store-path a-store)))
      (define-values (new-doc result) (f doc))
      (write-doc! (store-path a-store) new-doc)
      result)))
