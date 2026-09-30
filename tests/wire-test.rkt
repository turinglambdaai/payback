#lang racket/base

;; Wire validation tests: what RPC payloads are accepted, how they are
;; normalized, and which violations are rejected.

(require rackunit
         "../app/wire.rkt")

(define today "2026-09-30")
(define settings (hasheq 'currency "CNY"))

(define (valid-payload #:name [name "MacBook Pro"]
                       #:price [price 100000]
                       #:date [date "2026-06-22"]
                       #:extra [extra '()]
                       #:omit [omit '()])
  (apply hasheq
         (append
          (list 'name name 'priceMinor price 'purchaseDate date)
          (if (member 'icon omit) '() (list 'icon "💻"))
          extra)))

(test-case "valid payload normalizes"
  (define record
    (validate-new-device (valid-payload) settings today 'add-device))
  (check-equal? (hash-ref record 'name) "MacBook Pro")
  (check-equal? (hash-ref record 'priceMinor) 100000)
  (check-equal? (hash-ref record 'category) "other")
  (check-equal? (hash-ref record 'currency) "CNY")
  (check-equal? (hash-ref record 'willingPerDayMinor) 'null)
  (check-equal? (hash-ref record 'notes) ""))

(test-case "explicit category, currency and willing are kept"
  (define record
    (validate-new-device
     (valid-payload
      #:extra
      (list 'category "audio"
            'currency "USD"
            'willingPerDayMinor 300
            'notes "纪念日礼物"))
     settings
     today
     'add-device))
  (check-equal? (hash-ref record 'category) "audio")
  (check-equal? (hash-ref record 'currency) "USD")
  (check-equal? (hash-ref record 'willingPerDayMinor) 300)
  (check-equal? (hash-ref record 'notes) "纪念日礼物"))

(test-case "name is trimmed and must be non-empty"
  (check-equal? (hash-ref (validate-new-device (valid-payload #:name "  Kindle ") settings today 'add-device)
                          'name)
                "Kindle")
  (check-exn exn:fail?
             (lambda () (validate-new-device (valid-payload #:name "   ") settings today 'add-device))))

(test-case "rejections"
  (for ([payload (in-list
                  (list
                   (valid-payload #:price 0)                 ; zero price
                   (valid-payload #:price -5)                ; negative
                   (valid-payload #:price 100000.5)          ; not an integer
                   (valid-payload #:price 2000000000)        ; over cap
                   (valid-payload #:date "2026-10-01")       ; future
                   (valid-payload #:date "1989-12-31")       ; before floor
                   (valid-payload #:date "2026-02-30")       ; impossible
                   (valid-payload #:extra (list 'category "boat"))
                   (valid-payload #:extra (list 'currency "cny"))
                   (valid-payload #:extra (list 'willingPerDayMinor 0))
                   (valid-payload #:extra (list 'willingPerDayMinor -1))
                   (valid-payload #:extra (list 'notes (make-string 2001 #\x)))
                   (hasheq 'priceMinor 100 'purchaseDate today)))]) ; no name
    (check-exn exn:fail?
               (lambda () (validate-new-device payload settings today 'add-device))
               (format "~a must be rejected" payload))))

(test-case "client-supplied identity fields are ignored on create"
  (define record
    (validate-new-device
     (valid-payload #:extra (list 'id "d-fake"
                                  'createdAt "1999-01-01 00:00:00"
                                  'updatedAt "1999-01-01 00:00:00"))
     settings today 'add-device))
  (check-false (hash-has-key? record 'id))
  (check-false (hash-has-key? record 'createdAt)))

(test-case "device update keeps identity, patches fields"
  (define existing
    (hasheq 'id "d-1"
            'name "Old Name"
            'icon "📦"
            'category "other"
            'priceMinor 100
            'currency "CNY"
            'purchaseDate "2026-01-01"
            'willingPerDayMinor 'null
            'notes ""
            'createdAt "2026-01-01 10:00:00"
            'updatedAt "2026-01-01 10:00:00"))
  (define updated
    (validate-device-update
     (hasheq 'id "d-1" 'name "New Name" 'priceMinor 200
             'purchaseDate "2026-01-01" 'willingPerDayMinor 50)
     existing today 'update-device))
  (check-equal? (hash-ref updated 'name) "New Name")
  (check-equal? (hash-ref updated 'priceMinor) 200)
  (check-equal? (hash-ref updated 'willingPerDayMinor) 50)
  (check-equal? (hash-ref updated 'createdAt) "2026-01-01 10:00:00")
  (check-exn exn:fail?
             (lambda ()
               (validate-device-update
                (hasheq 'id "d-other" 'name "X" 'priceMinor 1
                        'purchaseDate "2026-01-01")
                existing today 'update-device))))

(test-case "settings patch accepts app-owned keys only"
  (define patch
    (validate-settings-patch
     (hasheq 'currency "USD" 'updateAutoCheck #f
             'updateBaseUrl "https://example.com/payback/"
             'rolloutBucket 5)          ; internal key -> rejected
     'save-settings))
  (check-equal? (hash-ref patch 'currency) "USD")
  (check-equal? (hash-ref patch 'updateAutoCheck) #f)
  (check-equal? (hash-ref patch 'updateBaseUrl) "https://example.com/payback/")
  (check-false (hash-has-key? patch 'rolloutBucket)))

(test-case "settings patch resets updateBaseUrl with null"
  (define patch
    (validate-settings-patch (hasheq 'updateBaseUrl 'null) 'save-settings))
  (check-equal? (hash-ref patch 'updateBaseUrl) 'null))

(test-case "settings patch rejections"
  (for ([payload (in-list
                  (list (hasheq 'currency "cny")
                        (hasheq 'currency "CN")
                        (hasheq 'currency 'null)
                        (hasheq 'updateAutoCheck "yes")
                        (hasheq 'updateAutoCheck 'null)
                        (hasheq 'updateBaseUrl "http://insecure.example")
                        (hasheq 'updateBaseUrl "not a url")))])
    (check-exn exn:fail?
               (lambda () (validate-settings-patch payload 'save-settings))
               (format "~a must be rejected" payload))))

(test-case "ids and timestamps have canonical shapes"
  (check-true (regexp-match? #px"^d-[0-9a-f]{10}$" (new-device-id)))
  (check-true (regexp-match? #px"^\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}$"
                             (now-timestamp))))
