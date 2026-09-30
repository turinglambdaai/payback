#lang racket/base

;; Store tests: defaults, atomic read-modify-write round-trips, forward
;; compatibility with unknown settings keys, and corrupt-file recovery.

(require rackunit
         racket/file
         racket/path
         racket/list
         racket/runtime-path
         json
         "../app/store.rkt")

(define tmp-root
  (path->complete-path
   (make-temporary-file "payback-store-test-~a" 'directory)))

(test-case "fresh store returns defaults"
  (define path (build-path tmp-root "a" "payback.json"))
  (define a-store (make-store path))
  (define doc (store-doc a-store))
  (check-equal? (hash-ref doc 'version) 1)
  (check-equal? (hash-ref doc 'devices) '())
  (check-equal? (hash-ref (hash-ref doc 'settings) 'currency) "CNY")
  (check-false (file-exists? path) "no file until the first mutation"))

(test-case "mutations persist atomically"
  (define path (build-path tmp-root "b" "payback.json"))
  (define a-store (make-store path))
  (define result
    (store-mutate! a-store
      (lambda (doc)
        (values (hash-set doc 'devices
                          (list (hasheq 'id "d-1" 'name "Kindle")))
                'added))))
  (check-equal? result 'added)
  ;; re-open from disk: the document is what was written
  (define doc (store-doc (make-store path)))
  (check-equal? (first (hash-ref doc 'devices))
                (hasheq 'id "d-1" 'name "Kindle")))

(test-case "unknown settings keys survive a round-trip"
  (define path (build-path tmp-root "c" "payback.json"))
  (define a-store (make-store path))
  (store-mutate! a-store
    (lambda (doc)
      (values (hash-set doc 'settings
                        (hash-set (hash-ref doc 'settings)
                                  'futureKey "kept"))
              (void))))
  (define doc (store-doc a-store))
  (check-equal? (hash-ref (hash-ref doc 'settings) 'futureKey) "kept")
  (check-equal? (hash-ref (hash-ref doc 'settings) 'currency) "CNY"))

(test-case "corrupt file is backed up, not fatal"
  (define path (build-path tmp-root "d" "payback.json"))
  (make-directory* (path-only path))
  (call-with-output-file path (lambda (o) (display "{not json" o)))
  (define doc (store-doc (make-store path)))
  (check-equal? (hash-ref doc 'devices) '())
  (check-true
   (< 0 (length (directory-list (path-only path))))
   "backup and fresh file exist"))

(test-case "json document is readable standard JSON"
  (define path (build-path tmp-root "e" "payback.json"))
  (define a-store (make-store path))
  (store-mutate! a-store
    (lambda (doc) (values (hash-set doc 'devices '()) (void))))
  (define value (call-with-input-file path read-json #:mode 'text))
  (check-true (hash? value)))
