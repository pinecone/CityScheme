;; Canonical float representation: one NaN, no -0.0, bit-equal numbers.

(define inf (/ 1 0))
(define nan (/ 0 0))

;; NaN is a number and every producer yields the same NaN.
($check (number? nan))
($check (eq? nan (- inf inf)))
($check (eq? nan (+ inf (- inf))))
($check (eq? nan (* 0 inf)))
($check (eq? nan (/ inf inf)))
($check (eq? nan (sqrt -1)))
($check (eq? nan (log -1)))
($check (eq? nan (expt -1 0.5)))
($check (eq? nan (asin 2)))
($check (eq? nan (acos 2)))
($check (eq? nan (string->number "nan")))
($check (eq? nan (truncate nan)))

;; NaN is equal to itself under every predicate.
($check (eq? nan nan))
($check (eqv? nan nan))
($check (equal? nan nan))
($check (= nan nan))
($check (equal? (vector nan) (vector (sqrt -1))))

;; NaN is unordered and unequal to every other number.
($check (not (< nan 1)))
($check (not (> nan 1)))
($check (not (<= nan nan)))
($check (not (>= nan nan)))
($check (not (= nan 1)))
($check (not (= nan inf)))

;; NaN is not integral.
($check (not (exact? nan)))
($check (not (integer? nan)))
($check (not (positive? nan)))
($check (not (negative? nan)))

;; There is only positive zero.
($check (eq? 0 -0.0))
($check (eq? 0 (- 0.0)))
($check (eq? 0 (- 0 0.0)))
($check (eq? 0 (* -1.0 0.0)))
($check (eq? 0 (/ -1 inf)))
($check (eq? 0 (round -0.4)))
($check (eq? 0 (truncate -0.5)))
($check (eq? 0 (truncate 0.5)))
($check (eq? 0 (truncate 0)))
($check (eq? 0 (truncate 5e-324)))
($check (eq? 0 (truncate -5e-324)))
($check (eq? 0 (truncate 0.9999999999999999)))
($check (eq? 0 (truncate -0.9999999999999999)))
($check (eq? 1 (truncate 1.0000000000000002)))
($check (eq? -1 (truncate -1.0000000000000002)))
($check (eq? 4503599627370495 (truncate 4503599627370495.5)))
($check (eq? -4503599627370495 (truncate -4503599627370495.5)))
($check (eq? 4503599627370496 (truncate 4503599627370496)))
($check (eq? -4503599627370496 (truncate -4503599627370496)))
($check (eq? 1.7976931348623157e308 (truncate 1.7976931348623157e308)))
($check (eq? -1.7976931348623157e308 (truncate -1.7976931348623157e308)))
($check (eq? 0 (ceiling -0.5)))
($check (eq? 0 (string->number "-0.0")))
($check (eq? inf (/ 1 (* -1.0 0.0))))
($check (eq? inf (/ 1 (- 0.0))))

;; Zeros and NaN survive a print round trip.
($check (string=? "0" (number->string (* -1.0 0.0))))
($check (string=? "nan" (number->string nan)))
($check (eq? inf (string->number "1e309")))

;; Infinities keep their sign and identity.
($check (eq? inf (+ inf inf)))
($check (eq? inf (truncate inf)))
($check (eq? (- inf) (truncate (- inf))))
($check (eq? (- inf) (- 0 inf)))
($check (exact? inf))
($check (integer? inf))

;; NaN is a legal hash key.
(define m (hashmap nan 'not-a-number 0.0 'zero))
($check (eq? 'not-a-number (ref m (/ 0 0))))
($check (eq? 'zero (ref m (* -1.0 0.0))))
(setf! m (- inf inf) 'overwritten)
($check (eq? 'overwritten (ref m nan)))
($check (ref (hashset nan) (sqrt -1)))
($check (not (ref (hashset 1) nan)))

;; NaN inside a key tuple.
(define by-tuple (hashmap (tuple nan 1) 'found))
($check (eq? 'found (ref by-tuple (tuple (/ 0 0) 1))))

(displayn "all float-canon checks passed")
