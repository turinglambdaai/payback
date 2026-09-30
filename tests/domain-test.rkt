#lang racket/base

;; Domain math tests: calendar edges, per-device payback math, milestone
;; ladders, and the portfolio summary.

(require rackunit
         racket/list
         "../app/domain.rkt")

(test-case "calendar round-trips"
  (for ([triple (in-list '((1970 1 1) (1990 1 1) (2000 3 1) (2024 2 29)
                           (2026 9 30) (2100 3 1) (1900 3 1)))])
    (define z (days-from-civil (car triple) (cadr triple) (caddr triple)))
    (define-values (y m d) (civil-from-days z))
    (check-equal? (list y m d) triple (format "~a" triple))))

(test-case "epoch anchor"
  (check-equal? (days-from-civil 1970 1 1) 0))

(test-case "leap-year arithmetic"
  (check-equal? (add-days "2024-02-28" 1) "2024-02-29")
  (check-equal? (add-days "2024-12-31" 1) "2025-01-01")
  (check-equal? (add-days "2023-02-28" 1) "2023-03-01")
  ;; 1900 and 2100 are not leap years
  (check-equal? (add-days "1900-02-28" 1) "1900-03-01")
  (check-equal? (add-days "2100-02-28" 1) "2100-03-01"))

(test-case "date validation"
  (check-true (date-string? "2026-09-30"))
  (check-false (date-string? "2026-02-30"))
  (check-false (date-string? "2026-13-01"))
  (check-false (date-string? "not-a-date"))
  (check-true (valid-date-range? "2026-09-30" "2026-09-30"))
  (check-false (valid-date-range? "2026-10-01" "2026-09-30"))  ; future
  (check-false (valid-date-range? "1989-12-31" "2026-09-30"))  ; before floor
  (check-true (valid-date-range? "1990-01-01" "2026-09-30")))

(test-case "days-between"
  (check-equal? (days-between "2026-01-01" "2026-01-01") 0)
  (check-equal? (days-between "2026-01-01" "2026-12-31") 364)
  (check-equal? (days-between "2024-01-01" "2025-01-01") 366))

(test-case "days-held counts purchase day"
  (check-equal? (days-held "2026-09-30" "2026-09-30") 1)
  (check-equal? (days-held "2026-09-29" "2026-09-30") 2)
  (check-equal? (days-held "2025-09-30" "2026-09-30") 366))

(define (make-device #:price [price 100000]
                     #:date [date "2026-06-23"]
                     #:willing [willing 'null]
                     #:id [id "d-test"])
  (hasheq 'id id
          'name "MacBook Pro"
          'icon "💻"
          'category "computer"
          'priceMinor price
          'currency "CNY"
          'purchaseDate date
          'willingPerDayMinor willing
          'notes ""
          'createdAt "2026-06-22 10:00:00"
          'updatedAt "2026-06-22 10:00:00"))

(define today "2026-09-30")  ; exactly 100 days of use from 2026-06-23

(test-case "per-device computed values"
  (define computed (device-computed (make-device) today))
  (check-equal? (hash-ref computed 'daysHeld) 100)
  (check-equal? (hash-ref computed 'costPerDayMinor) 1000.0)  ; exactly 10.00
  (check-equal? (hash-ref computed 'willingSet) #f)
  (check-equal? (hash-ref computed 'earnedMinor) 'null)
  (check-equal? (hash-ref computed 'paybackProgress) 'null)
  (check-equal? (hash-ref computed 'paybackEta) 'null)
  (check-equal? (hash-ref computed 'paidBack) #f))

(test-case "payback progress and eta"
  (define computed (device-computed (make-device #:willing 500) today))
  ;; 100 days * 5.00 = 500.00 of 1000.00 -> 50%
  (check-equal? (hash-ref computed 'willingSet) #t)
  (check-equal? (hash-ref computed 'earnedMinor) -50000.0)
  (check-equal? (hash-ref computed 'paybackProgress) 0.5)
  (check-equal? (hash-ref computed 'paybackEta)
                (add-days "2026-06-23" 200))  ; 200 yuan/days to cover 1000 yuan
  (check-equal? (hash-ref computed 'paidBack) #f)

  ;; willing == price/100 days: paid back exactly today
  (define even (device-computed (make-device #:willing 1000) today))
  (check-equal? (hash-ref even 'earnedMinor) 0.0)
  (check-equal? (hash-ref even 'paidBack) #t)
  (check-equal? (hash-ref even 'paybackEta) 'null)

  ;; long-held device is past break-even
  (define old (device-computed (make-device #:date "2020-01-01"
                                            #:willing 100)
                               today))
  (check-equal? (hash-ref old 'paidBack) #t))

(test-case "milestone ladder"
  (define ladder
    (device-computed (make-device #:willing 1000) today))
  (define achieved
    (for/list ([m (in-list (hash-ref ladder 'milestones))]
               #:when (hash-ref m 'achieved))
      (hash-ref m 'key)))
  (check-not-false (member "days-100" achieved) "100 days reached")
  (check-false (member "days-365" achieved) "365 days not reached")
  (check-not-false (member "paid-back" achieved) "paid back with willing=1000")

  ;; exactly 10.00 per day does not clear the <10 threshold
  (define boundary (device-computed (make-device) today))
  (define boundary-keys
    (for/list ([m (in-list (hash-ref boundary 'milestones))]
               #:when (hash-ref m 'achieved))
      (hash-ref m 'key)))
  (check-false (member "cpd-10" boundary-keys)
               "10.00/day must not satisfy < 10 milestone")
  (check-false (member "cpd-5" boundary-keys))

  ;; a 1-yuan device bought today clears every cost milestone at once
  (define cheap
    (device-computed (make-device #:price 100 #:date today) today))
  (define cheap-keys
    (for/list ([m (in-list (hash-ref cheap 'milestones))]
               #:when (hash-ref m 'achieved))
      (hash-ref m 'key)))
  ;; 1.00/day clears cpd-2 but exactly meets, not beats, cpd-1
  (check-not-false (member "cpd-2" cheap-keys))
  (check-false (member "cpd-1" cheap-keys))
  (check-false (member "days-100" cheap-keys)))

(test-case "milestone keys are a stable, complete ladder"
  (define ladder (milestone-ladder 0 1000000.0 #f))
  (check-equal?
   (map (lambda (m) (hash-ref m 'key)) ladder)
   (list "days-100" "days-365" "days-1000"
         "cpd-10" "cpd-5" "cpd-2" "cpd-1" "cpd-05"
         "paid-back")))

(test-case "portfolio summary"
  (define devices
    (list (make-device #:id "d-a")  ; 100 days, 1000.0/day
          (make-device #:id "d-b"
                       #:price 36500
                       #:date "2025-10-01"  ; 365 days -> 100.0/day
                       #:willing 100)))     ; earned exactly 0 -> paid back
  (define summary (portfolio-summary devices today))
  (check-equal? (hash-ref summary 'deviceCount) 2)
  (check-equal? (hash-ref summary 'totalSpentMinor) 136500)
  (check-equal? (hash-ref summary 'totalDaysHeld) 465)
  (check-equal? (hash-ref summary 'paidBackCount) 1)
  (check-equal? (hash-ref summary 'earnedTotalMinor) 0.0)
  (check-equal? (hash-ref summary 'bestDeviceId) "d-b")
  (check-equal? (hash-ref summary 'toughestDeviceId) "d-a"))

(test-case "portfolio summary empty"
  (define summary (portfolio-summary '() today))
  (check-equal? (hash-ref summary 'deviceCount) 0)
  (check-equal? (hash-ref summary 'totalSpentMinor) 0)
  (check-equal? (hash-ref summary 'bestDeviceId) 'null))
