#lang racket/base

;; Multi-device sync engine (docs/sync-design.md, data format v2).
;;
;; Every machine writes exactly one shard file — `devices-<machineId>.json`,
;; its "claim set" — into the active directory: the user-chosen sync folder
;; when sync is on, the plain local data directory when it is off. Merging
;; happens on read: union device records by id, decide each id and each
;; whitelisted settings key by last-writer-wins over (updatedAt, editedBy),
;; hide winners that carry a tombstone (deletedAt). The merge is
;; deterministic, order-independent and idempotent, so any dumb file sync
;; engine converges without data loss — no manifest, no server, no
;; conflict copies.
;;
;; This module never writes or deletes another machine's shard. Unparseable
;; or shape-broken files (half-uploaded by a sync engine) are skipped and
;; reported through `sync-state`.
;;
;; Lock order: the engine semaphore may be held while taking the local
;; store's semaphore — never the reverse. All state mutations go through
;; `sync-mutate!` / the *! functions below so that order is kept in one
;; place.

(require json
         racket/date
         racket/file
         racket/format
         racket/list
         racket/path
         racket/random
         racket/string
         "store.rkt"
         "wire.rkt")

(provide (struct-out sync-engine)
         make-sync-engine
         sync-init!
         sync-view
         sync-known-ids
         sync-machine-id
         sync-mutate!
         sync-claim-setting!
         sync-configure!
         sync-rename-machine!
         sync-refresh-if-changed!
         sync-state-payload
         sync-active-dir
         dir-string
         merge-shards
         scan-shard-files
         shard-file-name
         fresh-machine-id
         default-machine-name
         settings-sync-whitelist)

;; ---------- constants ----------

;; Settings that sync across machines, each key decided independently by
;; LWW (docs/sync-design.md). Everything else stays per-machine on purpose:
;; milestone celebrations are each device's own first-hand experience, and
;; update checks are local state.
(define settings-sync-whitelist '(currency))

(define shard-name-regexp #px"^devices-m-[0-9a-f]{8}\\.json$")
(define shard-loose-regexp #px"^devices-.+\\.json$")
(define max-path-chars 4096)

;; ---------- engine state ----------

(struct sync-engine
  (sema                ; guards everything below; may nest local-store takes
   local-store         ; store on <default-dir>/local.json
   machine-box         ; (cons "m-…" "MacBook Pro")
   sync-dir-box        ; #f | complete-path
   claims-box          ; hash id -> full device record (our claim set)
   setting-claims-box  ; hash symbol -> (hasheq value updatedAt editedBy)
   view-box            ; merged doc {version devices settings knownIds}
   revision-box        ; bumped whenever the merged state actually changes
   last-merge-box      ; timestamp string | #f
   ignored-box         ; list of skipped file names from the last scan
   machines-box        ; list of (hasheq id name) from shard metas
   fingerprint-box)    ; cheap directory-change detector
  #:transparent)

(define (make-sync-engine local-store)
  (sync-engine (make-semaphore 1)
               local-store
               (box #f) (box #f)
               (box (hash)) (box (hasheq))
               (box #f) (box 0) (box #f)
               (box '()) (box '()) (box '())))

(define (shard-file-name machine-id)
  (string-append "devices-" machine-id ".json"))

(define (fresh-machine-id)
  (string-append "m-"
                 (string-append*
                  (for/list ([byte (in-bytes (crypto-random-bytes 4))])
                    (~r byte #:base 16 #:min-width 2 #:pad-string "0")))))

(define (default-machine-name)
  (define host (system-type 'machine))
  (define name (if (string? host) (string-trim host) ""))
  (if (and (not (string=? name "")) (<= (string-length name) 100))
      name
      "Payback"))

;; ---------- claim shapes ----------

(define (plausible-claim? d)
  (and (hash? d)
       (string? (hash-ref d 'id #f))
       (string? (hash-ref d 'name #f))
       (exact-integer? (hash-ref d 'priceMinor #f))
       (string? (hash-ref d 'purchaseDate #f))))

(define (plausible-shard? value)
  (and (hash? value)
       (equal? 2 (hash-ref value 'version #f))
       (plausible-machine? (hash-ref value 'machine #f))
       (let ([devices (hash-ref value 'devices '())])
         (and (list? devices) (andmap plausible-claim? devices)))
       (hash? (hash-ref value 'settings (hasheq)))))

;; Readers tolerate claims written by older/other implementations: missing
;; metadata normalizes to "lost every tie" instead of failing the shard.
(define (normalize-claim d)
  (hash-set* d
             'updatedAt (hash-ref d 'updatedAt "")
             'editedBy (hash-ref d 'editedBy "")
             'deletedAt (hash-ref d 'deletedAt 'null)))

(define (tombstoned? d)
  (string? (hash-ref d 'deletedAt #f)))

;; LWW-Register pick: lexicographically larger (updatedAt, editedBy) wins.
;; Deterministic, commutative, associative — second key breaks exact ties
;; so every machine converges on the same winner even with skewed clocks.
(define (claim-lww a b)
  (define ua (hash-ref a 'updatedAt ""))
  (define ub (hash-ref b 'updatedAt ""))
  (cond
    [(string=? ua ub)
     (if (string>? (hash-ref a 'editedBy "") (hash-ref b 'editedBy "")) a b)]
    [else (if (string>? ua ub) a b)]))

(define (plausible-setting-claim? w)
  (and (hash? w)
       (string? (hash-ref w 'updatedAt #f))
       (string? (hash-ref w 'editedBy #f))))

;; ---------- pure merge (golden fixtures in tests/ run this directly) ----------

;; shards: list of parsed shard documents (as read from disk). Returns
;; (hasheq devices settings machines knownIds); devices are the live
;; (non-tombstoned) winners sorted by (createdAt, id) so every machine
;; lists identically regardless of scan order.
(define (merge-shards shards)
  (define device-winners
    ;; keyed by device id strings: MUST be an equal?-based hash — hasheq
    ;; would treat two parses of the same id as different keys and fork
    ;; the union
    (for/fold ([acc (hash)])
              ([shard (in-list shards)])
      (for/fold ([acc acc])
                ([raw (in-list (hash-ref shard 'devices '()))]
                 #:when (plausible-claim? raw))
        (define d (normalize-claim raw))
        (define id (hash-ref d 'id))
        (define incumbent (hash-ref acc id #f))
        (hash-set acc id (if incumbent (claim-lww incumbent d) d)))))
  (define live
    (sort (for/list ([d (in-hash-values device-winners)]
                     #:unless (tombstoned? d))
            d)
          (λ (a b)
            ;; deterministic view order: (createdAt, id), identical on
            ;; every machine regardless of scan order
            (define ka (cons (hash-ref a 'createdAt "") (hash-ref a 'id "")))
            (define kb (cons (hash-ref b 'createdAt "") (hash-ref b 'id "")))
            (cond [(string=? (car ka) (car kb))
                   (string<? (cdr ka) (cdr kb))]
                  [else (string<? (car ka) (car kb))]))))
  (define setting-winners
    (for/fold ([acc (hasheq)])
              ([shard (in-list shards)])
      (for/fold ([acc acc])
                ([key (in-list settings-sync-whitelist)])
        (define raw
          (hash-ref (hash-ref shard 'settings (hasheq)) key #f))
        (if (plausible-setting-claim? raw)
            ;; setting claims normalize (updatedAt, editedBy) only — no
            ;; tombstone field rides on a settings wrapper
            (let ([w (hash-set* raw
                                'updatedAt (hash-ref raw 'updatedAt "")
                                'editedBy (hash-ref raw 'editedBy ""))])
              (define incumbent (hash-ref acc key #f))
              (hash-set acc key (if incumbent (claim-lww incumbent w) w)))
            acc))))
  (define machines
    (sort
     (remove-duplicates
      (for/list ([shard (in-list shards)])
        (define m (hash-ref shard 'machine))
        (hasheq 'id (hash-ref m 'id) 'name (hash-ref m 'name)))
      #:key (λ (m) (hash-ref m 'id)))
     string<? #:key (λ (m) (hash-ref m 'id))))
  (hasheq 'devices live
          'settings setting-winners
          'machines machines
          'knownIds (sort (hash-keys device-winners) string<?)))

;; ---------- directory scan ----------

;; Returns (values valid-shards ignored-file-names). Files matching the
;; loose shard pattern that fail the strict name, the JSON parse or the
;; shape check are reported as ignored — never renamed, never deleted.
(define (scan-shard-files dir)
  (define entries
    (with-handlers ([exn:fail? (λ (_) '())])
      (map path->string (directory-list dir))))
  (for/fold ([shards '()] [ignored '()])
            ([name (in-list (sort entries string<?))]
             #:when (regexp-match? shard-loose-regexp name))
    (cond
      [(not (regexp-match? shard-name-regexp name))
       (values shards (cons name ignored))]
      [else
       (define full (build-path dir name))
       (define parsed
         (with-handlers ([exn:fail? (λ (_) #f)])
           (call-with-input-file full read-json #:mode 'text)))
       (cond
         [(plausible-shard? parsed)
          ;; the file name is authoritative for machine identity
          (define id-from-name
            (substring name 8 (- (string-length name) 5)))
          (define shard
            (hash-set parsed
                      'machine
                      (hash-set (hash-ref parsed 'machine) 'id id-from-name)))
          (values (cons shard shards) ignored)]
         [else (values shards (cons name ignored))])])))

(define (fingerprint dir)
  (with-handlers ([exn:fail? (λ (_) 'missing)])
    (for/list ([name (in-list (directory-list dir))]
               #:when (regexp-match? shard-loose-regexp (path->string name)))
      (define full (build-path dir name))
      (list (path->string name)
            (with-handlers ([exn:fail? (λ (_) -1)]) (file-size full))
            (with-handlers ([exn:fail? (λ (_) -1)])
              (file-or-directory-modify-seconds full))))))

;; ---------- active directory ----------

;; directory path as a wire string, without trailing separators — a path
;; that never got typed by a user must not depend on the platform's idea
;; of a directory separator (Windows dirs render with a trailing backslash)
(define (dir-string path)
  (let loop ([s (path->string path)])
    (if (and (> (string-length s) 1)
             (let ([c (string-ref s (sub1 (string-length s)))])
               (or (char=? c #\/) (char=? c #\\))))
        (loop (substring s 0 (sub1 (string-length s))))
        s)))

(define (default-dir engine)
  (define-values (base _name _dir?)
    (split-path (store-path (sync-engine-local-store engine))))
  (path->complete-path base))

(define (sync-active-dir engine)
  (or (unbox (sync-engine-sync-dir-box engine))
      (default-dir engine)))

;; ---------- v1 migration ----------

;; The active directory holds only the v1 single file and no valid shard:
;; import every device into our claim set (updatedAt preserved, editedBy is
;; us), carry local-only settings into the local document, seed the synced
;; settings claims from the file mtime so any newer remote claim wins. The
;; v1 file stays in place untouched as a backup.
(define (maybe-migrate-v1! engine)
  (define dir (sync-active-dir engine))
  (define v1-path (build-path dir "payback.json"))
  (define-values (shards _ignored) (scan-shard-files dir))
  (cond
    [(or (pair? shards) (not (file-exists? v1-path)))
     (values #f (hasheq) (hasheq))]
    [else
     (define v1
       (with-handlers ([exn:fail? (λ (_) #f)])
         (call-with-input-file v1-path read-json #:mode 'text)))
     (cond
       [(and (hash? v1)
             (list? (hash-ref v1 'devices '()))
             (andmap plausible-device? (hash-ref v1 'devices '()))
             (hash? (hash-ref v1 'settings (hasheq))))
        (define me (car (unbox (sync-engine-machine-box engine))))
        (define imported-at
          (with-handlers ([exn:fail? (λ (_) (now-timestamp))])
            (timestamp-from-seconds
             (file-or-directory-modify-seconds v1-path))))
        (define claims
          (for/hash ([raw (in-list (hash-ref v1 'devices))])
            (values
             (hash-ref raw 'id)
             (normalize-claim
              (hash-set* raw
                         'createdAt (hash-ref raw 'createdAt
                                              (hash-ref raw 'updatedAt ""))
                         'editedBy me
                         'deletedAt 'null)))))
        (define v1-settings (hash-ref v1 'settings))
        (define setting-claims
          (for/hasheq ([key (in-list settings-sync-whitelist)]
                       #:when (hash-has-key? v1-settings key))
            (values key
                    (hasheq 'value (hash-ref v1-settings key)
                            'updatedAt imported-at
                            'editedBy me))))
        (define local-overlay
          (for/hasheq ([key (in-list '(updateAutoCheck updateBaseUrl
                                       lastUpdateCheckAt rolloutBucket
                                       lastDigestAt seenMilestones))]
                       #:when (hash-has-key? v1-settings key))
            (values key (hash-ref v1-settings key))))
        (values claims setting-claims local-overlay)]
       ;; an unreadable v1 file is left alone — it is a backup, not a
       ;; database any more; the app just starts empty
       [else (values #f (hasheq) (hasheq))])]))

;; ---------- own shard ----------

(define (own-shard-doc engine
                       #:claims [claims (unbox (sync-engine-claims-box engine))]
                       #:settings [settings (unbox (sync-engine-setting-claims-box engine))])
  (define id-name (unbox (sync-engine-machine-box engine)))
  (hasheq 'version 2
          'machine (hasheq 'id (car id-name)
                           'name (cdr id-name)
                           'updatedAt (now-timestamp))
          ;; sorted by id: rewrites stay byte-similar, sync engines diff less
          'devices
          (sort (hash-values claims)
                string<? #:key (λ (d) (hash-ref d 'id)))
          'settings settings))

(define (write-own-shard! engine
                          [dir #f]
                          #:claims [claims #f]
                          #:settings [settings #f])
  (define id-name (unbox (sync-engine-machine-box engine)))
  (define target (or dir (sync-active-dir engine)))
  ;; a mutation passes its candidate state here (#24): the shard must
  ;; reach disk before memory adopts it, so a failed write leaves memory
  ;; equal to disk and a host retry cannot invent a second record
  (define doc
    (if (or claims settings)
        (own-shard-doc engine
                       #:claims (or claims (unbox (sync-engine-claims-box engine)))
                       #:settings (or settings (unbox (sync-engine-setting-claims-box engine))))
        (own-shard-doc engine)))
  (write-doc! (build-path target (shard-file-name (car id-name)))
              doc))

;; read our own shard into the claim set; an unreadable own shard takes the
;; corrupt path (renamed aside, claims start empty) — atomic replacement
;; makes that rare, but a sync engine can deliver a half file
(define (seed-claims-from-own-shard! engine)
  (define dir (sync-active-dir engine))
  (define id-name (unbox (sync-engine-machine-box engine)))
  (define path (build-path dir (shard-file-name (car id-name))))
  (cond
    [(not (file-exists? path)) (void)]
    [else
     (define parsed
       (with-handlers ([exn:fail? (λ (_) #f)])
         (call-with-input-file path read-json #:mode 'text)))
     (cond
       [(plausible-shard? parsed)
        (define claims
          (for/hash ([raw (in-list (hash-ref parsed 'devices '()))]
                     #:when (plausible-claim? raw))
            (values (hash-ref raw 'id) (normalize-claim raw))))
        (define setting-claims
          (for/hasheq ([(key raw) (in-hash (hash-ref parsed 'settings (hasheq)))]
                       #:when (and (memq key settings-sync-whitelist)
                                   (plausible-setting-claim? raw)))
            (values key (normalize-claim raw))))
        (set-box! (sync-engine-claims-box engine) claims)
        (set-box! (sync-engine-setting-claims-box engine) setting-claims)]
       [else
        (define backup
          (path-replace-extension path
                                  (format ".corrupt-~a.json" (current-seconds))))
        (with-handlers ([exn:fail? (λ (_) (void))])
          (rename-file-or-directory path backup #f))])]))

;; ---------- local document ----------

(define (persist-local! engine mutate)
  ;; mutate: local-doc -> new-local-doc ; caller must hold the engine
  ;; semaphore (lock order: engine -> local store)
  (store-mutate! (sync-engine-local-store engine)
    (lambda (doc) (values (mutate doc) (void)))))

;; ---------- init / refresh ----------

(define (sync-init! engine)
  (call-with-semaphore (sync-engine-sema engine)
    (lambda ()
      (define ldoc (store-doc (sync-engine-local-store engine)))
      (define machine (hash-ref ldoc 'machine 'null))
      (set-box! (sync-engine-machine-box engine)
                (if (plausible-machine? machine)
                    (cons (hash-ref machine 'id) (hash-ref machine 'name))
                    (cons (fresh-machine-id) (default-machine-name))))
      (define dir-spec (hash-ref ldoc 'syncDir 'null))
      (set-box! (sync-engine-sync-dir-box engine)
                (and (string? dir-spec)
                     (not (string=? dir-spec ""))
                     (with-handlers ([exn:fail? (λ (_) #f)])
                       (path->complete-path dir-spec))))
      (define-values (migrated-claims migrated-setting-claims local-overlay)
        (maybe-migrate-v1! engine))
      (if migrated-claims
          (begin
            (set-box! (sync-engine-claims-box engine) migrated-claims)
            (set-box! (sync-engine-setting-claims-box engine)
                      migrated-setting-claims)
            (write-own-shard! engine))
          (seed-claims-from-own-shard! engine))
      ;; persist identity (and any migrated local settings) immediately so
      ;; a crash right after startup cannot mint a second machine id
      (define id-name (unbox (sync-engine-machine-box engine)))
      (persist-local! engine
        (lambda (doc)
          (hash-set* doc
                     'machine (hasheq 'id (car id-name) 'name (cdr id-name))
                     'syncDir (if (unbox (sync-engine-sync-dir-box engine))
                                  (path->string
                                   (unbox (sync-engine-sync-dir-box engine)))
                                  'null)
                     'settings
                     (for/fold ([settings (hash-ref doc 'settings)])
                               ([(key value) (in-hash local-overlay)])
                       (hash-set settings key value)))))
      (refresh!/sema engine))))

(define (build-view engine merged)
  (define local-settings
    (hash-ref (store-doc (sync-engine-local-store engine)) 'settings))
  ;; fallback defaults under local document under synced winners: a fresh
  ;; install still has a currency to suggest, and synced claims override
  (define settings
    (for/fold ([acc (for/fold ([defaults (default-settings)])
                              ([(key value) (in-hash local-settings)])
                      (hash-set defaults key value))])
              ([(key wrapper) (in-hash (hash-ref merged 'settings))])
      (hash-set acc key (hash-ref wrapper 'value))))
  (hasheq 'version 1
          'devices (hash-ref merged 'devices)
          'settings settings
          'knownIds (hash-ref merged 'knownIds)))

(define (refresh!/sema engine #:bump-revision? [bump-revision? #t])
  (define dir (sync-active-dir engine))
  (define-values (shards ignored) (scan-shard-files dir))
  (define merged (merge-shards shards))
  (define view (build-view engine merged))
  (define changed?
    (not (and (equal? view (unbox (sync-engine-view-box engine)))
              (equal? (hash-ref merged 'machines)
                      (unbox (sync-engine-machines-box engine)))
              (equal? ignored (unbox (sync-engine-ignored-box engine))))))
  (set-box! (sync-engine-view-box engine) view)
  (set-box! (sync-engine-machines-box engine) (hash-ref merged 'machines))
  (set-box! (sync-engine-ignored-box engine) ignored)
  (set-box! (sync-engine-last-merge-box engine) (now-timestamp))
  (set-box! (sync-engine-fingerprint-box engine) (fingerprint dir))
  ;; local-only settings changes rebuild the view without announcing a
  ;; revision: the calling host already holds the fresh payload
  (when (and changed? bump-revision?)
    (set-box! (sync-engine-revision-box engine)
              (add1 (unbox (sync-engine-revision-box engine))))))

;; Cheap poll entry point: stat the shard files and re-merge only when the
;; directory actually changed. Hosts call this through `sync-state` on a
;; timer; stat polling is deliberately chosen over platform watchers
;; because network-mount change notifications are unreliable anyway
;; (docs/sync-design.md keeps a 30s floor for the same reason).
(define (sync-refresh-if-changed! engine)
  (call-with-semaphore (sync-engine-sema engine)
    (lambda ()
      (define fp (fingerprint (sync-active-dir engine)))
      (unless (equal? fp (unbox (sync-engine-fingerprint-box engine)))
        (refresh!/sema engine)))))

;; ---------- mutations ----------

;; `f` receives the merged view document and returns
;; (values change result). change is #f for pure reads (no shard write, no
;; re-merge) or (list 'device record) / (list 'setting key wrapper) to
;; claim a device record or a whitelisted settings key for this machine.
(define (sync-mutate! engine f)
  (call-with-semaphore (sync-engine-sema engine)
    (lambda ()
      (define doc (unbox (sync-engine-view-box engine)))
      (define-values (change result) (f doc))
      (when change
        (case (car change)
          [(device)
           (define record (normalize-claim (cadr change)))
           ;; commit-on-disk-first (#24): a failed shard write must leave
           ;; memory equal to disk, so the retry a host offers cannot
           ;; invent a second record for an outcome the user never saw
           (define next
             (hash-set (unbox (sync-engine-claims-box engine))
                       (hash-ref record 'id)
                       record))
           (write-own-shard! engine #:claims next)
           (set-box! (sync-engine-claims-box engine) next)
           (refresh!/sema engine)]
          [(setting)
           (define wrapper (caddr change))
           ;; same commit-on-disk-first ordering as the device branch (#24)
           (define next
             (hash-set (unbox (sync-engine-setting-claims-box engine))
                       (cadr change)
                       (hash-set* wrapper
                                  'updatedAt (hash-ref wrapper 'updatedAt "")
                                  'editedBy (hash-ref wrapper 'editedBy ""))))
           (write-own-shard! engine #:settings next)
           (set-box! (sync-engine-setting-claims-box engine) next)
           (refresh!/sema engine)]
          ;; the local document changed underneath (milestone acks, digest
          ;; stamp): no shard write, but the cached view settings are stale
          [(local-settings)
           (refresh!/sema engine #:bump-revision? #f)]))
      result)))

(define (sync-claim-setting! engine key value)
  (define wrapper
    (hasheq 'value value
            'updatedAt (now-timestamp)
            'editedBy (car (unbox (sync-engine-machine-box engine)))))
  (sync-mutate! engine (λ (_doc) (values (list 'setting key wrapper) wrapper))))

;; ---------- configuration ----------

(define (check-sync-dir-spec spec)
  (unless (and (string? spec)
               (<= 1 (string-length spec) max-path-chars)
               (absolute-path? spec))
    (error 'configure-sync
           "field 'dir' must be an absolute folder path (up to ~a characters)"
           max-path-chars))
  (simplify-path (path->complete-path spec)))

;; dir-spec: an absolute folder path (created if missing) or null to turn
;; sync off. The own shard is written into the target folder *before* the
;; choice is persisted, so an unwritable folder fails without flipping the
;; engine into a broken state.
(define (sync-configure! engine dir-spec)
  (call-with-semaphore (sync-engine-sema engine)
    (lambda ()
      (cond
        [(or (eq? dir-spec 'null) (eq? dir-spec #f)
             (and (string? dir-spec) (string=? (string-trim dir-spec) "")))
         ;; bring our claims home so the next launch finds them in the
         ;; default directory
         (write-own-shard! engine (default-dir engine))
         (set-box! (sync-engine-sync-dir-box engine) #f)
         (persist-local! engine
           (lambda (doc) (hash-set doc 'syncDir 'null)))]
        [(string? dir-spec)
         (define path (check-sync-dir-spec (string-trim dir-spec)))
         (make-directory* path)
         (write-own-shard! engine path)
         (set-box! (sync-engine-sync-dir-box engine) path)
         (persist-local! engine
           (lambda (doc) (hash-set doc 'syncDir (path->string path))))]
        [else
         (error 'configure-sync "field 'dir' must be a folder path or null")])
      (refresh!/sema engine)
      (sync-state-payload/sema engine))))

(define (sync-rename-machine! engine name)
  (unless (and (string? name)
               (<= 1 (string-length (string-trim name)) 100))
    (error 'rename-machine "field 'name' must be 1-100 characters"))
  (call-with-semaphore (sync-engine-sema engine)
    (lambda ()
      (define id-name (unbox (sync-engine-machine-box engine)))
      (set-box! (sync-engine-machine-box engine)
                (cons (car id-name) (string-trim name)))
      (persist-local! engine
        (lambda (doc)
          (hash-set doc
                    'machine (hasheq 'id (car id-name)
                                     'name (string-trim name)))))
      (write-own-shard! engine)
      (refresh!/sema engine)
      (sync-state-payload/sema engine))))

;; ---------- accessors / payloads ----------

(define (sync-view engine)
  (unbox (sync-engine-view-box engine)))

(define (sync-known-ids engine)
  (hash-ref (sync-view engine) 'knownIds '()))

;; machine id only; safe to call while the engine semaphore is held (plain
;; box read, no locking)
(define (sync-machine-id engine)
  (car (unbox (sync-engine-machine-box engine))))

(define (sync-state-payload/sema engine)
  (define id-name (unbox (sync-engine-machine-box engine)))
  ;; this machine always appears in the peer list, with the local display
  ;; name winning over a stale shard meta; peers come from their shard metas
  (define machines
    (sort (cons (hasheq 'id (car id-name) 'name (cdr id-name))
                (for/list ([m (in-list (unbox (sync-engine-machines-box engine)))]
                           #:unless (string=? (hash-ref m 'id) (car id-name)))
                  m))
          string<? #:key (λ (m) (hash-ref m 'id))))
  (hasheq 'enabled (and (unbox (sync-engine-sync-dir-box engine)) #t)
          'dir (dir-string (sync-active-dir engine))
          'defaultDir (dir-string (default-dir engine))
          'machine (hasheq 'id (car id-name) 'name (cdr id-name))
          'machines machines
          'lastMergeAt (or (unbox (sync-engine-last-merge-box engine)) 'null)
          'revision (unbox (sync-engine-revision-box engine))
          'ignoredFiles (unbox (sync-engine-ignored-box engine))))

(define (sync-state-payload engine)
  (call-with-semaphore (sync-engine-sema engine)
    (lambda () (sync-state-payload/sema engine))))
