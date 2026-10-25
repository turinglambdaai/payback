#lang racket/base

;; Sync engine tests (docs/sync-design.md). Two layers:
;;
;; 1. Golden fixtures in fixtures/sync/ — plain JSON data files (the future
;;    Swift/Kotlin mobile cores must be able to run the same fixtures) —
;;    covering union, LWW winners, editedBy tie-breaks, tombstones,
;;    settings whitelist, scan-order independence and idempotence.
;; 2. Engine tests on scratch directories — v1 migration, claim-set
;;    discipline, external-change detection, configure-sync, rename.

(require json
         rackunit
         racket/file
         racket/list
         racket/path
         racket/runtime-path
         "../app/store.rkt"
         "../app/sync.rkt")

(define tmp-root
  (path->complete-path
   (make-temporary-file "payback-sync-test-~a" 'directory)))

(define-runtime-path fixture-dir "fixtures/sync")

(define (read-json-file path)
  (call-with-input-file path read-json #:mode 'text))

;; ---------- helpers ----------

(define (view-live-ids merged)
  (map (λ (d) (hash-ref d 'id)) (hash-ref merged 'devices)))

(define (winner-for merged id)
  (for/first ([d (in-list (hash-ref merged 'devices))]
              #:when (string=? (hash-ref d 'id) id))
    d))

(define (shard->claims shard)
  ;; a shard document turned back into a claim-carrying shard (used by the
  ;; idempotence check: "everyone echoes the merged winners" must be a
  ;; fixed point)
  (hasheq 'version 2
          'machine (hash-ref shard 'machine)
          'devices (hash-ref shard 'devices)
          'settings (hash-ref shard 'settings)))

;; ---------- golden fixtures ----------

(define shard-a (read-json-file (build-path fixture-dir "shard-a.json")))
(define shard-b (read-json-file (build-path fixture-dir "shard-b.json")))
(define shard-c (read-json-file (build-path fixture-dir "shard-c.json")))
(define expected (read-json-file (build-path fixture-dir "expected-view.json")))

(test-case "golden: merge matches the expected view"
  (define merged (merge-shards (list shard-a shard-b shard-c)))
  (check-equal? (view-live-ids merged)
                (hash-ref expected 'liveOrder)
                "live devices, in the deterministic (createdAt, id) order")
  (for ([(id-symbol want) (in-hash (hash-ref expected 'deviceById))])
    (define id (symbol->string id-symbol))
    (define winner (winner-for merged id))
    (check-equal? (hash-ref winner 'name) (hash-ref want 'name)
                  (format "~a winner name" id))
    (check-equal? (hash-ref winner 'updatedAt) (hash-ref want 'updatedAt)
                  (format "~a winner updatedAt" id))
    (check-equal? (hash-ref winner 'editedBy) (hash-ref want 'editedBy)
                  (format "~a winner editedBy" id))
    (check-equal? (hash-ref winner 'deletedAt #f) (hash-ref want 'deletedAt)
                  (format "~a tombstone state" id)))
  (for ([(key want) (in-hash (hash-ref expected 'settings))])
    (check-equal? (hash-ref (hash-ref merged 'settings) key #f)
                  (hasheq 'value (hash-ref want 'value)
                          'updatedAt (hash-ref want 'updatedAt)
                          'editedBy (hash-ref want 'editedBy))
                  (format "settings winner ~a" key)))
  (check-equal? (hash-ref merged 'machines) (hash-ref expected 'machines))
  (check-equal? (hash-ref merged 'knownIds) (hash-ref expected 'knownIds)
                "known ids include tombstoned devices"))

(test-case "golden: union survives single-shard degenerate cases"
  (check-equal? (view-live-ids (merge-shards '())) '())
  (check-equal? (length (view-live-ids (merge-shards (list shard-a)))) 3)
  (check-equal? (length (view-live-ids (merge-shards (list shard-c)))) 1))

(test-case "golden: merge is independent of scan order"
  (define permutations
    (list
     (list shard-a shard-b shard-c)
     (list shard-a shard-c shard-b)
     (list shard-b shard-a shard-c)
     (list shard-b shard-c shard-a)
     (list shard-c shard-a shard-b)
     (list shard-c shard-b shard-a)))
  (define baseline (merge-shards (first permutations)))
  (for ([perms (in-list (rest permutations))])
    (check-equal? (merge-shards perms) baseline "order must not matter")))

(test-case "golden: merging an echo of the winners is a fixed point"
  ;; state-based CRDT convergence: if every machine rewrote its shard to
  ;; carry exactly the merged winners, the view must not move
  (define baseline (merge-shards (list shard-a shard-b shard-c)))
  (define echo-shards
    (for/list ([m (in-list (hash-ref baseline 'machines))])
      (hasheq 'version 2
              'machine (hash-set
                        (hash-ref (case (hash-ref m 'id)
                                    [("m-aaaaaaaa") shard-a]
                                    [("m-bbbbbbbb") shard-b]
                                    [else shard-c])
                                  'machine)
                        'id (hash-ref m 'id))
              'devices (hash-ref baseline 'devices)
              'settings (hash-ref baseline 'settings))))
  (define echoed (merge-shards echo-shards))
  ;; the view itself must not move; the echo shards carry no tombstones,
  ;; so knownIds legitimately shrinks to the live set
  (check-equal? (hash-ref echoed 'devices) (hash-ref baseline 'devices))
  (check-equal? (hash-ref echoed 'settings) (hash-ref baseline 'settings))
  (check-equal? (hash-ref echoed 'machines) (hash-ref baseline 'machines)))

(test-case "golden: malformed claims do not sink a shard"
  (define broken
    (hasheq 'version 2
            'machine (hasheq 'id "m-dddddddd" 'name "Broken" 'updatedAt "x")
            'devices (list
                      ;; missing id: skipped entirely
                      (hasheq 'name "No Id" 'priceMinor 1
                              'purchaseDate "2024-01-01")
                      ;; missing priceMinor: skipped
                      (hasheq 'id "d-zzzzzzzzzz" 'name "No Price"
                              'purchaseDate "2024-01-01"))
            'settings (hasheq)))
  (define merged (merge-shards (list shard-a broken)))
  (check-false (member "d-zzzzzzzzzz" (hash-ref merged 'knownIds))))

;; ---------- scan ----------

(test-case "scan ignores half files, dot files and conflict copies"
  (define dir (build-path tmp-root "scan"))
  (make-directory* dir)
  (define (write-shard name body)
    (call-with-output-file (build-path dir name)
      (lambda (o) (display body o)) #:exists 'replace))
  (write-shard "devices-m-1234abcd.json"
               (jsexpr->string shard-a))
  (write-shard "devices-m-1234abcd.json.tmp" "{\"version\":2}")     ; half upload
  (write-shard "devices-m-feedface (conflicted copy).json" "{}")    ; Dropbox conflict copy
  (write-shard "devices-m-notvalid.json" "{}")                      ; wrong machine id shape
  (write-shard ".devices-m-1234abcd.json" "{}")                     ; dotfile
  (define-values (shards ignored) (scan-shard-files dir))
  (check-equal? (length shards) 1 "exactly the one valid shard parses")
  (check-equal? (hash-ref (hash-ref (first shards) 'machine) 'id)
                "m-1234abcd"
                "machine identity comes from the file name")
  (check-equal?
   (sort ignored string<?)
   (sort (list "devices-m-feedface (conflicted copy).json"
               "devices-m-notvalid.json")
         string<?)
   "dotfile and .tmp skipped silently, the rest reported"))

(test-case "scan on a missing directory is empty, not fatal"
  (define-values (shards ignored)
    (scan-shard-files (build-path tmp-root "does-not-exist")))
  (check-equal? shards '())
  (check-equal? ignored '()))

;; ---------- engine ----------

(define (make-test-engine dir)
  (define eng (make-sync-engine (make-store (build-path dir "local.json"))))
  (sync-init! eng)
  eng)

(test-case "fresh engine mints a machine id and starts empty"
  (define dir (build-path tmp-root "fresh"))
  (define eng (make-test-engine dir))
  (check-true (regexp-match? #px"^m-[0-9a-f]{8}$" (sync-machine-id eng)))
  (check-equal? (hash-ref (sync-view eng) 'devices) '())
  (check-equal? (hash-ref (hash-ref (sync-view eng) 'settings) 'currency)
                "CNY"
                "fallback default currency before any claim exists")
  (define ldoc (store-doc (make-store (build-path dir "local.json"))))
  (check-equal? (hash-ref (hash-ref ldoc 'machine) 'id)
                (sync-machine-id eng)
                "machine identity persisted on first init")
  (check-equal? (hash-ref (hash-ref ldoc 'machine) 'name)
                (hash-ref (hash-ref (sync-state-payload eng) 'machine) 'name)))

(test-case "v1 payback.json migrates once into the claim set"
  (define dir (build-path tmp-root "migrate"))
  (make-directory* dir)
  (define v1
    (hasheq 'version 1
            'devices
            (list (hasheq 'id "d-old1" 'name "Migrating Kindle" 'icon "📚"
                          'category "other" 'priceMinor 100000
                          'currency "CNY" 'purchaseDate "2024-01-01"
                          'willingPerDayMinor 'null 'notes ""
                          'createdAt "2024-01-01 09:00:00"
                          'updatedAt "2025-01-01 09:00:00"))
            'settings
            (hasheq 'currency "EUR"
                    'updateAutoCheck #f
                    'updateBaseUrl "https://example.invalid/feed"
                    'lastUpdateCheckAt 12345
                    'rolloutBucket 7
                    'seenMilestones (hasheq 'd-old1 '("days-100"))
                    'licenseKey "PB1.migrated")))
  (call-with-output-file (build-path dir "payback.json")
    (lambda (o) (write-json v1 o)))
  (define eng (make-test-engine dir))
  ;; devices surfaced through the merged view…
  (check-equal? (view-live-ids (sync-view eng)) '("d-old1"))
  ;; …claim-ported into our own shard (payback.json stays a backup)…
  (define shard-path
    (build-path dir (shard-file-name (sync-machine-id eng))))
  (check-true (file-exists? shard-path))
  (check-true (file-exists? (build-path dir "payback.json")))
  (define shard (read-json-file shard-path))
  (check-equal? (hash-ref (hash-ref shard 'machine) 'id)
                (sync-machine-id eng))
  (check-equal? (hash-ref (first (hash-ref shard 'devices)) 'updatedAt)
                "2025-01-01 09:00:00"
                "v1 updatedAt is preserved on import")
  ;; …synced settings became claims…
  (check-equal? (hash-ref (hash-ref (sync-view eng) 'settings) 'currency) "EUR")
  ;; …and local-only settings moved into the local document; the legacy
  ;; license key is neither synced nor local — it drops out of the active
  ;; documents, while the untouched v1 file keeps it as backup history (#20)
  (define ldoc (store-doc (make-store (build-path dir "local.json"))))
  (check-equal? (hash-ref (hash-ref ldoc 'settings) 'updateAutoCheck) #f)
  (check-equal? (hash-ref (hash-ref ldoc 'settings) 'seenMilestones)
                (hasheq 'd-old1 '("days-100")))
  (check-false (hash-has-key? (hash-ref ldoc 'settings) 'licenseKey))
  (check-false (hash-has-key? (hash-ref (sync-view eng) 'settings) 'licenseKey))
  (define v1-backup (read-json-file (build-path dir "payback.json")))
  (check-equal? (hash-ref (hash-ref v1-backup 'settings) 'licenseKey)
                "PB1.migrated"
                "the untouched v1 file keeps the legacy key as history")
  ;; a second init must not re-import (claims already carry the device)
  (sync-init! eng)
  (check-equal? (view-live-ids (sync-view eng)) '("d-old1")))

(test-case "mutations claim records into our shard only"
  (define dir (build-path tmp-root "claims"))
  (make-directory* dir)
  (define foreign-id "m-feedface")
  (define foreign-shard-path
    (build-path dir (shard-file-name foreign-id)))
  (define foreign
    (hasheq 'version 2
            'machine (hasheq 'id foreign-id 'name "Foreign" 'updatedAt "t")
            'devices
            (list (hasheq 'id "d-foreign" 'name "Foreign Device" 'icon "📦"
                          'category "other" 'priceMinor 500
                          'currency "CNY" 'purchaseDate "2025-01-01"
                          'willingPerDayMinor 'null 'notes ""
                          'createdAt "2025-01-01 09:00:00"
                          'updatedAt "2025-01-01 09:00:00"
                          'editedBy foreign-id 'deletedAt 'null))
            'settings (hasheq)))
  (call-with-output-file foreign-shard-path
    (lambda (o) (write-json foreign o)))
  (define eng (make-test-engine dir))
  (check-equal? (view-live-ids (sync-view eng)) '("d-foreign"))
  (define before-revision
    (hash-ref (sync-state-payload eng) 'revision))
  ;; edit the foreign device: it moves into OUR claim set (takeover), the
  ;; foreign shard stays byte-identical — we never write other machines
  (define updated-at
    (sync-mutate! eng
      (lambda (doc)
        (define existing
          (for/first ([d (in-list (hash-ref doc 'devices))]
                      #:when (string=? (hash-ref d 'id) "d-foreign"))
            d))
        ;; stamp like the backend does: fresh updatedAt + editedBy is what
        ;; makes our takeover claim win the LWW
        (define full
          (hash-set* existing
                     'name "Edited Here"
                     'updatedAt "2026-10-11 10:00:00"
                     'editedBy (sync-machine-id eng)))
        (values (list 'device full) 'ok))))
  (check-equal? updated-at 'ok)
  (define own-path
    (build-path dir (shard-file-name (sync-machine-id eng))))
  (check-true (file-exists? own-path) "our shard exists after the claim")
  (define own-shard (read-json-file own-path))
  (check-equal? (hash-ref (first (hash-ref own-shard 'devices)) 'name)
                "Edited Here")
  (check-equal? (hash-ref (first (hash-ref own-shard 'devices)) 'editedBy)
                (sync-machine-id eng))
  (check-equal? (read-json-file foreign-shard-path) foreign
                "foreign shard untouched")
  (check-equal? (hash-ref (sync-state-payload eng) 'revision)
                (add1 before-revision)
                "our own mutation bumps the revision")
  ;; tombstone hides the device but it stays a known id
  (sync-mutate! eng
    (lambda (doc)
      (define existing
        (for/first ([d (in-list (hash-ref doc 'devices))]
                    #:when (string=? (hash-ref d 'id) "d-foreign"))
          d))
      (values (list 'device (hash-set existing 'deletedAt "2026-10-11 11:11:11"))
              (void))))
  (check-equal? (view-live-ids (sync-view eng)) '()
                "tombstoned winner hides from the view")
  (check-not-false (member "d-foreign" (sync-known-ids eng))
                   "tombstoned ids are never handed out again"))

(test-case "external shard changes are picked up by the poll"
  (define dir (build-path tmp-root "poll"))
  (make-directory* dir)
  (define eng (make-test-engine dir))
  (sync-refresh-if-changed! eng) ; primes the fingerprint
  (define rev0 (hash-ref (sync-state-payload eng) 'revision))
  (sleep 0.05)
  (define foreign
    (hasheq 'version 2
            'machine (hasheq 'id "m-99999999" 'name "Late Arrival" 'updatedAt "t")
            'devices
            (list (hasheq 'id "d-late" 'name "Late Device" 'icon "📦"
                          'category "other" 'priceMinor 900
                          'currency "CNY" 'purchaseDate "2025-02-02"
                          'willingPerDayMinor 'null 'notes ""
                          'createdAt "2025-02-02 09:00:00"
                          'updatedAt "2025-02-02 09:00:00"
                          'editedBy "m-99999999" 'deletedAt 'null))
            'settings (hasheq)))
  (call-with-output-file
      (build-path dir (shard-file-name "m-99999999"))
    (lambda (o) (write-json foreign o)))
  (sync-refresh-if-changed! eng)
  (check-equal? (view-live-ids (sync-view eng)) '("d-late")
                "external shard merged after the poll")
  (check-equal? (hash-ref (sync-state-payload eng) 'revision) (add1 rev0))
  (define machine-names
    (map (λ (m) (hash-ref m 'name))
         (hash-ref (sync-state-payload eng) 'machines)))
  (check-not-false (member "Late Arrival" machine-names)
                   "machines list reports the new peer")
  (check-equal? (length machine-names) 2)
  ;; a second poll without changes is a no-op
  (sync-refresh-if-changed! eng)
  (check-equal? (hash-ref (sync-state-payload eng) 'revision) (add1 rev0)))

(test-case "configure-sync points the engine at a folder and back"
  (define dir (build-path tmp-root "configure"))
  (make-directory* dir)
  (define sync-dir (build-path tmp-root "sync-folder"))
  (define eng (make-test-engine dir))
  (sync-mutate! eng
    (lambda (_doc)
      (values (list 'device
                    (hasheq 'id "d-home" 'name "Home Device" 'icon "📦"
                            'category "other" 'priceMinor 100
                            'currency "CNY" 'purchaseDate "2025-01-01"
                            'willingPerDayMinor 'null 'notes ""
                            'createdAt "2025-01-01 09:00:00"
                            'updatedAt "2025-01-01 09:00:00"
                            'editedBy (sync-machine-id eng)
                            'deletedAt 'null))
              (void))))
  (define state-on (sync-configure! eng (path->string sync-dir)))
  (check-true (hash-ref state-on 'enabled))
  (check-equal? (hash-ref state-on 'dir) (path->string sync-dir))
  (check-true (file-exists?
               (build-path sync-dir
                           (shard-file-name (sync-machine-id eng))))
              "our shard moved into the sync folder")
  (check-equal? (view-live-ids (sync-view eng)) '("d-home")
                "claims survive the move")
  (define state-off (sync-configure! eng 'null))
  (check-false (hash-ref state-off 'enabled))
  (check-equal? (hash-ref state-off 'dir) (dir-string dir))
  (check-true (file-exists?
               (build-path dir (shard-file-name (sync-machine-id eng))))
              "claims came home for the next launch")
  (check-equal? (view-live-ids (sync-view eng)) '("d-home"))
  ;; an unwritable target must fail without flipping the engine: the
  ;; parent "directory" is a plain file, which fails on every platform
  ;; (a /dev/null path would silently succeed on Windows, where "null"
  ;; is just a legal directory name under C:\dev)
  (define plain-file (build-path tmp-root "not-a-dir"))
  (call-with-output-file plain-file (lambda (o) (display "x" o))
                         #:exists 'truncate)
  (check-exn exn:fail?
             (lambda ()
               (sync-configure! eng
                                (path->string (build-path plain-file "sub")))))
  (check-false (hash-ref (sync-state-payload eng) 'enabled))
  ;; a relative path is rejected with a user-visible error
  (check-exn exn:fail? (lambda () (sync-configure! eng "relative/path"))))

(test-case "rename-machine updates identity, shard meta and state"
  (define dir (build-path tmp-root "rename"))
  (make-directory* dir)
  (define eng (make-test-engine dir))
  (define state (sync-rename-machine! eng "  Studio Mac  "))
  (check-equal? (hash-ref (hash-ref state 'machine) 'name) "Studio Mac"
                "name is trimmed")
  (define ldoc (store-doc (make-store (build-path dir "local.json"))))
  (check-equal? (hash-ref (hash-ref ldoc 'machine) 'name) "Studio Mac")
  (check-exn exn:fail? (lambda () (sync-rename-machine! eng "")))
  (check-exn exn:fail? (lambda () (sync-rename-machine! eng 'null))))

(test-case "a failed shard write leaves memory equal to disk"
  ;; issue #24: hosts offer a retry after a failed save; that retry must
  ;; not invent a second record, so a failed disk write rolls the mutation
  ;; back before the caller ever sees the error
  (define dir (build-path tmp-root "durability"))
  (make-directory* dir)
  (define eng (make-test-engine dir))
  (sync-init! eng)
  (define (mutate-record id)
    (sync-mutate! eng
      (lambda (_doc)
        (values
         (list 'device
               (hasheq 'id id 'name "Durability Probe" 'icon "📦"
                       'category "other" 'priceMinor 100 'currency "CNY"
                       'purchaseDate "2026-01-01" 'willingPerDayMinor 'null
                       'notes "" 'createdAt "2026-01-01 09:00:00"
                       'updatedAt "2026-01-01 09:00:00" 'editedBy "m-x"
                       'deletedAt 'null))
         (void)))))
  (file-or-directory-permissions dir #o555)
  (check-exn exn:fail? (λ () (mutate-record "d-lost"))
             "the write failure surfaces to the caller as an error")
  (file-or-directory-permissions dir #o755)
  (check-false
   (hash-has-key? (unbox (sync-engine-claims-box eng)) "d-lost")
   "the failed mutation left no in-memory trace")
  (mutate-record "d-retried")
  (check-equal?
   (length
    (filter (λ (d) (string=? (hash-ref d 'name) "Durability Probe"))
            (hash-values (unbox (sync-engine-claims-box eng)))))
   1
   "the retry records exactly one device"))

(test-case "settings claims ride the LWW whitelist"
  (define dir (build-path tmp-root "settings"))
  (make-directory* dir)
  (define eng (make-test-engine dir))
  (sync-claim-setting! eng 'currency "JPY")
  (check-equal? (hash-ref (hash-ref (sync-view eng) 'settings) 'currency)
                "JPY")
  (define shard
    (read-json-file
     (build-path dir (shard-file-name (sync-machine-id eng)))))
  (define wrapper (hash-ref (hash-ref shard 'settings) 'currency))
  (check-equal? (hash-ref wrapper 'value) "JPY")
  (check-equal? (hash-ref wrapper 'editedBy) (sync-machine-id eng))
  (check-true (string? (hash-ref wrapper 'updatedAt))
              "the claim lands with (updatedAt, editedBy) metadata"))
