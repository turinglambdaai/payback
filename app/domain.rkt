#lang racket/base

;; Pure domain math for Payback: date arithmetic, per-device daily cost and
;; payback progress, milestone ladders, and the portfolio summary. No I/O and
;; no Rivet dependencies so everything here is directly unit-testable.
;;
;; Money convention (docs/data-format.md): stored amounts are integer minor
;; units (fen/cents); computed rates are floats, also in minor units.
;; Optional values are 'null (JSON null), never #f, so computed payloads can
;; go over the wire unchanged; real JSON booleans only appear where a value
;; is genuinely boolean.

(require racket/format
         racket/list
         racket/string)

(provide today-string
         date-string?
         min-date-string
         valid-date-range?
         days-from-civil
         civil-from-days
         days-between
         add-days
         days-held
         device-computed
         milestone-ladder
         portfolio-summary
         portfolio-summary/computed)

;; ---------- dates (proleptic Gregorian, timezone-free) ----------

;; Howard Hinnant's days_from_civil. Years are CE and always positive here,
;; so Racket's quotient/remainder match the C++ truncating division.
(define (days-from-civil y m d)
  (define yy (if (<= m 2) (sub1 y) y))
  (define mm (if (<= m 2) (+ m 9) (- m 3)))
  (define era (quotient yy 400))
  (define yoe (- yy (* 400 era)))
  (define doy (quotient (+ (* 153 mm) 2) 5))
  (define doe (+ (* 365 yoe) (quotient yoe 4) (- (quotient yoe 100)) doy (- d 1)))
  (+ (* 146097 era) doe -719468))

(define (civil-from-days z)
  (define zd (+ z 719468))
  (define era (quotient zd 146097))
  (define doe (- zd (* 146097 era)))
  (define yoe
    (quotient (+ (- doe (quotient doe 1460)) (quotient doe 36524)
                 (- (quotient doe 146096)))
              365))
  (define y (+ yoe (* 400 era)))
  (define doy
    (- doe (+ (* 365 yoe) (quotient yoe 4) (- (quotient yoe 100)))))
  (define mp (quotient (+ (* 5 doy) 2) 153))
  (define d (+ (- doy (quotient (+ (* 153 mp) 2) 5)) 1))
  (define m (if (< mp 10) (+ mp 3) (- mp 9)))
  (values (if (<= m 2) (add1 y) y) m d))

(define (pad2 n) (~r n #:min-width 2 #:pad-string "0"))

(define (civil->string y m d)
  (format "~a-~a-~a" y (pad2 m) (pad2 d)))

(define (today-string)
  (define now (seconds->date (current-seconds)))
  (civil->string (date-year now) (date-month now) (date-day now)))

(define (date-string? s)
  (and (string? s)
       (regexp-match? #px"^\\d{4}-\\d{2}-\\d{2}$" s)
       (with-handlers ([exn:fail? (lambda (_) #f)])
         (define y (string->number (substring s 0 4)))
         (define m (string->number (substring s 5 7)))
         (define d (string->number (substring s 8 10)))
         ;; round-trip rejects impossible dates such as 2026-02-31
         (define-values (ry rm rd) (civil-from-days (days-from-civil y m d)))
         (and (= ry y) (= rm m) (= rd d)))))

(define min-date-string "1990-01-01")

;; purchase dates may not lie in the future relative to `today`
(define (valid-date-range? s today)
  (and (date-string? s)
       (string>=? s min-date-string)
       (string<=? s today)))

(define (date-parts s)
  (values (string->number (substring s 0 4))
          (string->number (substring s 5 7))
          (string->number (substring s 8 10))))

(define (days-between from-string to-string)
  (define-values (fy fm fd) (date-parts from-string))
  (define-values (ty tm td) (date-parts to-string))
  (- (days-from-civil ty tm td) (days-from-civil fy fm fd)))

(define (add-days date-string n)
  (define-values (y m d) (date-parts date-string))
  (define-values (ry rm rd) (civil-from-days (+ (days-from-civil y m d) n)))
  (civil->string ry rm rd))

;; ---------- per-device computed values ----------

;; The day of purchase counts as day one: a device bought yesterday has
;; already served two days by today.
(define (days-held purchase-date today)
  (max 1 (add1 (days-between purchase-date today))))

(define (device-computed device today)
  (define purchase-date (hash-ref device 'purchaseDate))
  (define price (hash-ref device 'priceMinor))
  (define days (days-held purchase-date today))
  (define cost-per-day (exact->inexact (/ price days)))
  (define willing (hash-ref device 'willingPerDayMinor #f))
  (define willing-set?
    (and (exact-integer? willing) (> willing 0)))
  (define earned
    (if willing-set?
        (exact->inexact (- (* days willing) price))
        'null))
  (define progress
    (if willing-set?
        (exact->inexact (/ (* days willing) price))
        'null))
  (define paid-back? (and willing-set? (>= earned 0)))
  (define eta
    (cond
      [(not willing-set?) 'null]
      [paid-back? 'null]
      [else (add-days purchase-date (ceiling (/ price willing)))]))
  (hasheq 'daysHeld days
          'costPerDayMinor cost-per-day
          'willingSet willing-set?
          'earnedMinor earned
          'paybackProgress progress
          'paybackEta eta
          'paidBack paid-back?
          'milestones (milestone-ladder days cost-per-day paid-back?)))

;; ---------- milestones ----------

;; Fixed thresholds; keys are stable wire identifiers that native hosts
;; localize (docs/data-format.md). Every milestone is always reported with an
;; achieved flag so the detail view can render the full ladder.
(define day-milestones
  '(("days-100" . 100) ("days-365" . 365) ("days-1000" . 1000)))

;; thresholds in minor units (cpd-05 = daily cost below 0.50)
(define cpd-milestones
  '(("cpd-10" . 1000) ("cpd-5" . 500) ("cpd-2" . 200)
    ("cpd-1" . 100) ("cpd-05" . 50)))

(define (milestone-ladder days cost-per-day paid-back?)
  (append
   (for/list ([m (in-list day-milestones)])
     (hasheq 'key (car m) 'achieved (>= days (cdr m))))
   (for/list ([m (in-list cpd-milestones)])
     (hasheq 'key (car m) 'achieved (< cost-per-day (cdr m))))
   (list (hasheq 'key "paid-back" 'achieved paid-back?))))

;; ---------- portfolio summary ----------

(define (summary-from devices computed-list)
  (define total-spent
    (for/sum ([d (in-list devices)]) (hash-ref d 'priceMinor)))
  (define total-days
    (for/sum ([c (in-list computed-list)]) (hash-ref c 'daysHeld)))
  (define earned-total
    (for/sum ([c (in-list computed-list)])
      (define e (hash-ref c 'earnedMinor))
      (if (and (real? e) (>= e 0)) e 0)))
  (define paid-back-count
    (for/sum ([c (in-list computed-list)])
      (if (hash-ref c 'paidBack) 1 0)))
  (define ranked
    (for/list ([d (in-list devices)]
               [c (in-list computed-list)])
      (cons (hash-ref d 'id) (hash-ref c 'costPerDayMinor))))
  (hasheq 'deviceCount (length devices)
          'totalSpentMinor (or total-spent 0)
          'totalDaysHeld (or total-days 0)
          'avgCostPerDayMinor
          (if (or (zero? total-days) (not total-spent))
              0.0
              (exact->inexact (/ total-spent total-days)))
          'earnedTotalMinor (exact->inexact earned-total)
          'paidBackCount paid-back-count
          'bestDeviceId (if (null? ranked) 'null (car (argmin cdr ranked)))
          'toughestDeviceId (if (null? ranked) 'null (car (argmax cdr ranked)))))

(define (portfolio-summary devices today)
  (summary-from devices
                (for/list ([d (in-list devices)]) (device-computed d today))))

;; Same summary over devices that already carry their 'computed block (the
;; load-all/digest path) — no per-device recomputation.
(define (portfolio-summary/computed devices)
  (summary-from devices
                (for/list ([d (in-list devices)]) (hash-ref d 'computed))))
