#lang racket/base

;; Online update support for Payback, built on rivet/distribution. The
;; backend verifies and downloads signed update artifacts; the native host
;; owns installation (docs/UPDATE.md). A check downloads and verifies the
;; Ed25519-signed update-manifest.json — the family single-file wrapper
;; carrying every platform and architecture; the actual DMG/MSI download
;; runs on a background thread with progress published to a state box that
;; the UI polls through the `update-state` RPC (RVT1 events are
;; thread-local, so a background thread cannot emit them directly).

(require crypto
         crypto/all
         net/base64
         net/url
         rivet/distribution
         racket/file
         racket/format
         racket/match
         racket/port
         racket/string
         "store.rkt"
         "version.rkt")

(provide platform-symbol
         architecture-symbol
         installer-extension
         manifest-url
         destination-path
         copy-with-progress!
         update-state-snapshot
         reset-update-state!
         perform-check!
         start-download!
         rollout-bucket)

;; rivet release tooling emits these exact symbols into update manifests
(define (platform-symbol)
  (case (system-type 'os)
    [(macosx) 'macos]
    [(windows) 'windows]
    [else 'linux]))

(define (architecture-symbol)
  (case (system-type 'arch)
    [(aarch64 arm64) 'arm64]
    [else 'x64]))

(define (installer-extension)
  (case (system-type 'os)
    [(macosx) ".dmg"]
    [(windows) ".msi"]
    [else ".tar.gz"]))

(define maximum-download-bytes (* 800 1024 1024))

;; ---------- public key ----------

;; rivet/distribution already ran (use-all-factories!) at module load; the
;; embedded DER is the public half of keys/update-ed25519-private.der, which
;; never ships (docs/updates.md).
(define (embedded-public-key)
  (datum->pk-key (base64-string->bytes update-public-key-b64)
                 'SubjectPublicKeyInfo))

;; ---------- shared update state (UI-visible) ----------

;; phase: idle | checking | downloading | downloaded | error
(define update-state
  (box (hasheq 'phase "idle"
               'percent 0
               'message 'null
               'downloadedPath 'null
               'availableVersion 'null)))

(define candidate-box (box #f))
(define worker-thread-box (box #f))

(define (state-set! key value)
  (set-box! update-state (hash-set (unbox update-state) key value)))

(define (update-state-snapshot)
  (unbox update-state))

(define (reset-update-state!)
  (set-box! candidate-box #f)
  (set-box! update-state
            (hasheq 'phase "idle"
                    'percent 0
                    'message 'null
                    'downloadedPath 'null
                    'availableVersion 'null)))

;; ---------- manifest URL ----------

(define (manifest-url settings)
  ;; Single-file family feed (taskly baseline): one signed wrapper per
  ;; release, every platform × architecture inside. The channel still gates
  ;; selection (select-update matches app-channel against the manifest).
  (define base
    (let ([configured (hash-ref settings 'updateBaseUrl 'null)])
      (if (and (string? configured) (not (string=? configured "")))
          configured
          default-update-base-url)))
  (string-append (string-trim base "/" #:right? #t)
                 "/update-manifest.json"))

;; ---------- check ----------

;; Returns a wire jsexpr describing the outcome; also refreshes
;; "lastUpdateCheckAt" (epoch seconds) in the store on every completed check.
(define (perform-check! store settings)
  (state-set! 'phase "checking")
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (state-set! 'phase "error")
          (state-set! 'message (exn-message e))
          (hasheq 'status "error" 'message (exn-message e)))])
    (define manifest
      (fetch-update-manifest (manifest-url settings)
                             (embedded-public-key)
                             #:key-id update-key-id
                             ;; release assets answer with a 302 to the CDN
                             #:redirections 10))
    (define config
      (updater-config app-identifier
                      app-version
                      app-channel
                      (platform-symbol)
                      (architecture-symbol)
                      (embedded-public-key)
                      update-key-id
                      (rollout-bucket store settings)
                      maximum-download-bytes))
    (define candidate (select-update config manifest))
    (store-mutate! store
                   (lambda (doc)
                     (values
                      (settings-with doc 'lastUpdateCheckAt (current-seconds))
                      (void))))
    (cond
      [candidate
       (set-box! candidate-box candidate)
       (define artifact (update-candidate-artifact candidate))
       (state-set! 'phase "idle")
       (state-set! 'availableVersion
                   (update-manifest-version (update-candidate-manifest candidate)))
       (hasheq 'status "available"
               'currentVersion app-version
               'availableVersion
               (update-manifest-version (update-candidate-manifest candidate))
               'build (update-manifest-build (update-candidate-manifest candidate))
               'publishedAt
               (update-manifest-published-at (update-candidate-manifest candidate))
               'installer (symbol->string (update-artifact-installer artifact))
               'sizeBytes (update-artifact-size artifact))]
      [else
       (state-set! 'phase "idle")
       (state-set! 'availableVersion 'null)
       (hasheq 'status "up-to-date" 'currentVersion app-version)])))

;; helper producing a document with one settings key replaced
(define (settings-with doc key value)
  (hash-set doc 'settings
            (hash-set (hash-ref doc 'settings) key value)))

;; rollout bucket: stable random 0..99 assigned on first check so staged
;; rollouts are sticky per installation
(define (rollout-bucket store settings)
  (define existing (hash-ref settings 'rolloutBucket 'null))
  (cond
    [(and (exact-integer? existing) (<= 0 existing 99)) existing]
    [else
     (define bucket (random 100))
     (store-mutate! store
                    (lambda (doc)
                      (values (settings-with doc 'rolloutBucket bucket)
                              (void))))
     bucket]))

;; ---------- download ----------

(define (destination-path data-dir candidate)
  (define version
    (update-manifest-version (update-candidate-manifest candidate)))
  (build-path data-dir
              "updates"
              (string-append app-display-name "-" version
                             (installer-extension))))

;; copy with progress; same limits as rivet's download-update but publishes
;; integer percent changes to the state box while streaming
(define (copy-with-progress! in out total)
  (define buffer (make-bytes 65536))
  (let loop ([done 0] [last-percent -1])
    (define count (read-bytes-avail! buffer in))
    (cond
      [(eof-object? count) done]
      [else
       (write-bytes buffer out 0 count)
       (define next (+ done count))
       (define percent
         (if (> total 0)
             (min 100 (quotient (* next 100) total))
             0))
       (when (> percent last-percent)
         (state-set! 'percent percent))
       (loop next percent)])))

(define (download-with-progress! config candidate destination)
  (define artifact (update-candidate-artifact candidate))
  (define total (update-artifact-size artifact))
  (when (> total (updater-config-maximum-download-bytes config))
    (error 'download-update "signed artifact size exceeds the download limit"))
  (make-parent-directory* destination)
  (define temporary (path-add-extension destination #".partial"))
  (when (file-exists? temporary) (delete-file temporary))
  (define in
    (get-pure-port (string->url (update-artifact-url artifact))
                   '("User-Agent: Payback-Updater/1")
                   #:redirections 10))
  (dynamic-wind
    void
    (lambda ()
      (call-with-output-file temporary
        #:exists 'truncate/replace
        #:mode 'binary
        (lambda (out) (copy-with-progress! in out total))))
    (lambda () (close-input-port in)))
  ;; size + SHA-256 against the signed manifest before the file is trusted
  (verify-update-artifact! candidate temporary)
  (rename-file-or-directory temporary destination #t)
  destination)

(define (start-download! store settings data-dir)
  (define worker (unbox worker-thread-box))
  (when (and worker (thread-running? worker))
    (error 'start-download! "an update download is already running"))
  (define candidate (unbox candidate-box))
  (unless candidate
    (error 'start-download! "no update is available; run a check first"))
  (state-set! 'phase "downloading")
  (state-set! 'percent 0)
  (state-set! 'message 'null)
  (define config
    (updater-config app-identifier
                    app-version
                    app-channel
                    (platform-symbol)
                    (architecture-symbol)
                    (embedded-public-key)
                    update-key-id
                    (rollout-bucket store settings)
                    maximum-download-bytes))
  (define destination (destination-path data-dir candidate))
  (set-box! worker-thread-box
            (thread
             (lambda ()
               (with-handlers
                   ([exn:fail?
                     (lambda (e)
                       (state-set! 'phase "error")
                       (state-set! 'message (exn-message e)))])
                 (define path
                   (download-with-progress! config candidate destination))
                 (state-set! 'phase "downloaded")
                 (state-set! 'percent 100)
                 (state-set! 'downloadedPath (path->string path)))))))
