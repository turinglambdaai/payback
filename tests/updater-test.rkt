#lang racket/base

;; Updater tests: platform mapping, manifest URL construction, download
;; progress accounting, and the full offline trust chain — craft a manifest,
;; sign it with a throwaway Ed25519 key, verify it, and select updates the
;; same way the live checker does.

(require crypto
         crypto/all
         rackunit
         net/base64
         net/url
         rivet/distribution
         racket/file
         racket/list
         racket/port
         "../app/store.rkt"
         "../app/updater.rkt"
         "../app/version.rkt")

(use-all-factories!)

(test-case "platform symbols match rivet release manifests"
  (case (system-type 'os)
    [(macosx) (check-equal? (platform-symbol) 'macos)]
    [(windows) (check-equal? (platform-symbol) 'windows)]
    [else (check-equal? (platform-symbol) 'linux)])
  (check-not-false (memq (architecture-symbol) '(arm64 x64))))

(test-case "installer extension follows platform"
  (check-not-false
   (member (installer-extension) '(".dmg" ".msi" ".pkg"))
   "known installer extension"))

(test-case "manifest url joins base and channel"
  (check-equal? (manifest-url (hasheq 'updateBaseUrl 'null))
                (string-append default-update-base-url "/update-stable.json"))
  (check-equal? (manifest-url (hasheq 'updateBaseUrl "https://dl.example/pb/"))
                "https://dl.example/pb/update-stable.json")
  (check-equal? (manifest-url (hasheq 'updateBaseUrl "https://dl.example/pb"))
                "https://dl.example/pb/update-stable.json"))

(test-case "download progress copies bytes and reports percent"
  (reset-update-state!)
  (define payload (make-bytes 250000 7))
  (define out (open-output-bytes))
  (copy-with-progress! (open-input-bytes payload) out 250000)
  (check-equal? (bytes-length (get-output-bytes out)) 250000)
  (check-equal? (hash-ref (update-state-snapshot) 'percent) 100)
  ;; percent tracks the declared total, not the end of input
  (reset-update-state!)
  (define short-out (open-output-bytes))
  (copy-with-progress! (open-input-bytes payload) short-out 1000000)
  (check-equal? (hash-ref (update-state-snapshot) 'percent) 25)
  (reset-update-state!))

(test-case "signed manifest verifies and selects updates"
  ;; throwaway keypair: same DER formats the release pipeline uses
  (define priv (generate-private-key 'eddsa '((curve ed25519))))
  (define priv-der (pk-key->datum priv 'OneAsymmetricKey))
  ;; derive the public half through the library: the rkt-private datum
  ;; element order differs between generated and DER-imported keys
  (define pub (datum->pk-key (pk-key->datum priv 'rkt-public) 'rkt-public))
  (define pub-der (pk-key->datum pub 'SubjectPublicKeyInfo))

  (define tmp (make-temporary-file "payback-updater-~a" 'directory))
  (define priv-path (build-path tmp "priv.der"))
  (define manifest-path (build-path tmp "update-stable.json"))
  (call-with-output-file priv-path
    (lambda (o) (write-bytes priv-der o)) #:exists 'truncate/replace)

  (define artifact
    (update-artifact 'macos 'arm64
                     "https://downloads.example.com/payback/Payback-1.1.0.dmg"
                     "0000000000000000000000000000000000000000000000000000000000000000"
                     123456789 'dmg '()))
  (define manifest
    (update-manifest app-identifier "1.1.0" 2 'stable
                     "2026-09-30T09:00:00Z" "0.0.0" "1.0.0" #t 100
                     (list artifact)))
  (call-with-output-file manifest-path
    (lambda (o) (write-signed-manifest manifest
                                       (read-ed25519-private-key priv-path)
                                       "test-key" o)
      (newline o)))

  ;; tamper check first: a modified payload must fail verification
  (define signed-bytes (file->bytes manifest-path))
  (define tampered
    (bytes-append
     (subbytes signed-bytes 0 40)
     #"9"
     (subbytes signed-bytes 41)))
  (check-exn exn:fail?
             (lambda ()
               (verify-signed-manifest (open-input-bytes tampered)
                                       (datum->pk-key pub-der 'SubjectPublicKeyInfo)
                                       #:key-id "test-key")))

  (define verified
    (verify-signed-manifest (open-input-bytes signed-bytes)
                            (datum->pk-key pub-der 'SubjectPublicKeyInfo)
                            #:key-id "test-key"))
  (check-equal? (update-manifest-version verified) "1.1.0")

  ;; selection policy: newer version for this app/channel/platform wins
  (define config
    (updater-config app-identifier "1.0.0" 'stable 'macos 'arm64
                    (datum->pk-key pub-der 'SubjectPublicKeyInfo)
                    "test-key" 42 800000000))
  (check-true (update-candidate? (select-update config verified)))
  ;; same version: nothing to do
  (define same
    (struct-copy update-manifest verified [version "1.0.0"]))
  (check-false (select-update config same))
  ;; other channel is invisible
  (define beta
    (struct-copy update-manifest verified [channel 'beta]))
  (check-false (select-update config beta))
  ;; rollout: bucket 42 is excluded when rollout is 42 (bucket >= rollout)
  (define partial
    (struct-copy update-manifest verified [rollout 42]))
  (check-false (select-update config partial))
  (define wider
    (struct-copy update-manifest verified [rollout 43]))
  (check-true (update-candidate? (select-update config wider)))
  ;; other application identity must fail loudly
  (define foreign
    (struct-copy update-manifest verified
                 [application-id "com.other.app"]))
  (check-exn exn:fail?
             (lambda () (select-update config foreign))))

(test-case "embedded public key decodes"
  ;; the shipped key must stay a valid SubjectPublicKeyInfo DER blob
  (check-true
   (pk-key? (datum->pk-key (base64-string->bytes update-public-key-b64)
                           'SubjectPublicKeyInfo))))

(test-case "rollout bucket is sticky and persisted"
  (define a-store
    (make-store
     (build-path (make-temporary-file "payback-bucket-~a" 'directory)
                 "payback.json")))
  (define first-bucket
    (rollout-bucket a-store (hash-ref (store-doc a-store) 'settings)))
  (check-true (and (exact-integer? first-bucket) (<= 0 first-bucket 99)))
  (check-equal? (hash-ref (hash-ref (store-doc a-store) 'settings)
                          'rolloutBucket)
                first-bucket
                "the assigned bucket is written back to the store")
  ;; a later check reads the stored bucket: staged rollouts stay sticky
  (check-equal?
   (rollout-bucket a-store (hash-ref (store-doc a-store) 'settings))
   first-bucket))

(test-case "artifact verification enforces signed size and hash"
  (define tmp (make-temporary-file "payback-verify-~a" 'directory))
  (define file-path (build-path tmp "installer.bin"))
  (define payload #"payback-installer-bytes")
  (call-with-output-file file-path
    (lambda (o) (write-bytes payload o)) #:exists 'truncate/replace)
  (define (candidate-for sha size)
    ;; the manifest half is not consulted by verification
    (update-candidate #f
                      (update-artifact 'macos 'arm64
                                       "https://downloads.example.com/x.dmg"
                                       sha size 'dmg '())))
  (define good
    (candidate-for (sha256-file/hex file-path) (bytes-length payload)))
  (check-equal? (verify-update-artifact! good file-path) file-path)
  (check-exn exn:fail?
             (lambda ()
               (verify-update-artifact!
                (candidate-for (sha256-file/hex file-path)
                               (add1 (bytes-length payload)))
                file-path))
             "size must match the signed manifest")
  (call-with-output-file file-path
    (lambda (o) (write-bytes #"tampered" o)) #:exists 'truncate/replace)
  (check-exn exn:fail?
             (lambda () (verify-update-artifact! good file-path))
             "hash must match the signed manifest"))
