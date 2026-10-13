#lang racket/base

;; Online update support for Payback, built on rivet/distribution. The
;; backend verifies and downloads signed update artifacts; the native host
;; owns installation (docs/UPDATE.md). A check downloads and verifies the
;; Ed25519-signed update-manifest.json — the family single-file wrapper
;; carrying every platform and architecture; the actual DMG/MSI download
;; runs on a background thread with progress published to a state box that
;; the UI polls through the `update-state` RPC (RVT1 events are
;; thread-local, so a background thread cannot emit them directly).

;; crypto goes through rivet/distribution only (same constraint as
;; license.rkt): crypto/all would pull the nettle factory's GMP FFI into
;; the embedded bundle, killing startup on machines without libgmp.

(require net/base64
         rivet/distribution
         racket/file
         racket/format
         racket/match
         racket/string
         "store.rkt"
         "version.rkt")

(provide platform-symbol
         architecture-symbol
         installer-extension
         manifest-url
         destination-path
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

;; rivet/distribution pins the libcrypto factory at its module load; the
;; embedded DER is the public half of keys/update-ed25519-private.der, which
;; never ships (docs/updates.md).
(define (embedded-public-key)
  (bytes->ed25519-public-key (base64-string->bytes update-public-key-b64)))

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

;; Download watchdog bounds. rivet's copy loop waits on read-bytes-avail!
;; with no timeout, so a connection that stalls without RST/FIN parks the
;; worker thread forever — visibly "stuck at 100%" (all bytes had arrived;
;; the loop was still waiting for EOF). Two finite bounds keep the phase
;; machine honest: no byte progress for the stall limit, or wall clock past
;; the total limit, breaks the worker (rivet's partial-file cleanup runs,
;; and the worker reports phase=error itself) so the UI always gets a
;; terminal state.
(define download-stall-limit-seconds 120)
(define download-total-limit-seconds (* 30 60))
(define last-progress-seconds (box 0))

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
                             #:key-id update-key-id))
    ;; fetch-update-manifest follows GitHub release-asset redirections
    ;; internally; it accepts only #:key-id and #:maximum-bytes.
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

;; rivet's download-update owns the whole artifact path — redirect-following
;; fetch (the rivet#153 302 shape), signed-size cap, .partial cleanup on
;; any failure, SHA-256 + size verification against the signed manifest,
;; atomic rename — so the wrapper only maps its byte progress onto the
;; integer percent the UI polls, and feeds the watchdog's stall clock on
;; every report.
(define (download-with-progress! config candidate destination)
  (define total (update-artifact-size (update-candidate-artifact candidate)))
  (download-update
   config candidate destination
   #:on-progress
   (lambda (done)
     (set-box! last-progress-seconds (current-seconds))
     (state-set! 'percent
                 (if (> total 0)
                     (min 100 (quotient (* done 100) total))
                     0)))))

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
  (set-box! last-progress-seconds (current-seconds))
  (define worker-thread
    (thread
     (lambda ()
       ;; The watchdog breaks a stalled worker; exn:break is not an
       ;; exn:fail, so it needs its own arm — either way rivet's partial
       ;; cleanup removes the .partial and the phase lands on error
       ;; (never mid-flight).
       (with-handlers
           ([(lambda (e) (or (exn:fail? e) (exn:break? e)))
             (lambda (e)
               (state-set! 'phase "error")
               (state-set! 'message
                           (if (exn:break? e)
                               "update download stalled or timed out"
                               (exn-message e))))])
         (define path
           (download-with-progress! config candidate destination))
         (state-set! 'phase "downloaded")
         (state-set! 'percent 100)
         (state-set! 'downloadedPath (path->string path))))))
  (set-box! worker-thread-box worker-thread)
  ;; Watchdog: exits once the worker finishes on its own; otherwise a stall
  ;; (no byte progress) or the total wall-clock limit breaks the worker so
  ;; phase never rests on "downloading" indefinitely.
  (define download-started (current-seconds))
  (thread
   (lambda ()
     (let loop ()
       (cond
         [(sync/timeout download-stall-limit-seconds
                        (thread-dead-evt worker-thread))
          (void)]
         [(> (- (current-seconds) (unbox last-progress-seconds))
             download-stall-limit-seconds)
          (break-thread worker-thread)]
         [(> (- (current-seconds) download-started)
             download-total-limit-seconds)
          (break-thread worker-thread)]
         [else (loop)]))))
  (void))
