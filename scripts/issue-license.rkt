#lang racket/base

;; Vendor tool: issue a Payback Pro license token for one customer.
;;
;;   racket scripts/issue-license.rkt \
;;     --key keys/license-ed25519-private.der \
;;     --subject "customer@example.com" \
;;     [--expiry 2027-12-31]
;;
;; Prints the PB1 token the customer pastes into the app's activation
;; dialog (macOS menu: Activate Payback Pro; Windows toolbar: Pro button).
;; The token is offline: no server, no activation call-home. Fulfillment is
;; whatever sells it — the token goes into the confirmation email.

(require crypto
         crypto/all
         racket/cmdline
         racket/file
         racket/string
         rivet/distribution
         "../app/license.rkt")

(use-all-factories!)

(define key-path #f)
(define subject #f)
(define expiry #f)

(command-line
 #:program "issue-license"
 #:once-each
 ["--key" k "private key DER path" (set! key-path k)]
 ["--subject" s "customer name/email embedded in the license" (set! subject s)]
 ["--expiry" e "optional expiry, YYYY-MM-DD (omit = perpetual)" (set! expiry e)])

(unless key-path (error 'issue-license "--key is required"))
(unless subject (error 'issue-license "--subject is required"))
(unless (file-exists? key-path)
  (error 'issue-license "private key not found: ~a" key-path))
(when expiry
  (unless (regexp-match? #px"^\\d{4}-\\d{2}-\\d{2}$" expiry)
    (error 'issue-license "--expiry must be YYYY-MM-DD")))

(displayln
 (issue-pro-token #:private-key (read-ed25519-private-key key-path)
                  #:subject (string-trim subject)
                  #:expiry expiry))
