;; Number operations

;; Basic arithmetic
($check (= 0 (+ 0 0)))
($check (= 0 (- 5 5)))
($check (= 1 (* 1 1)))
($check (= 2 (/ 10 5)))

;; Variadic arithmetic
($check (= 10 (+ 1 2 3 4)))
($check (= 24 (* 1 2 3 4)))

;; Unary minus
($check (= -5 (- 5)))
($check (= 5 (- -5)))

;; Comparisons
($check (<= 1 1))
($check (<= 1 2))
($check (not (<= 2 1)))
($check (>= 2 2))
($check (>= 3 2))
($check (not (>= 1 2)))

;; Floor, abs, modulo
($check (= 3 (floor 3.7)))
($check (= 5 (abs -5)))
($check (= 5 (abs 5)))
($check (= 1 (modulo 7 3)))
($check (= 0 (modulo 6 3)))
($check (= -1 (modulo -7 -3)))
($check (= 2 (modulo -7 3)))
($check (= -2 (modulo 7 -3)))
($check (= 1.5 (modulo 7.5 2.0)))
($check (= 0.5 (modulo -7.5 2.0)))
($check (< (abs (- 1.794 (modulo -6.206 2.0))) 1e-9))

;; Quotient, remainder
($check (= 2 (quotient 7 3)))
($check (= -2 (quotient -7 3)))
($check (= -2 (quotient 7 -3)))
($check (= 1 (remainder 7 3)))
($check (= -1 (remainder -7 3)))
($check (= 1 (remainder 7 -3)))
($check (= 3.0 (quotient 7.5 2.0)))
($check (= 1.5 (remainder 7.5 2.0)))

;; quotient + remainder reconstruct dividend (truncation division)
($check (= 7 (+ (* (quotient 7 3) 3) (remainder 7 3))))
($check (= -7 (+ (* (quotient -7 3) 3) (remainder -7 3))))
($check (= 7 (+ (* (quotient 7 -3) -3) (remainder 7 -3))))

;; Exponentiation
($check (= 8 (expt 2 3)))
($check (= 1 (expt 5 0)))

;; Max, min
($check (= 5 (max 1 5 3)))
($check (= 1 (min 1 5 3)))

;; Predicates (integer? and zero? are defined in standard library)
($check (zero? 0))
($check (not (zero? 1)))

;; Hex literals, positive and negative
($check (= 255 0xff))
($check (= -255 -0xff))
($check (= 31 0x1f))
($check (= -31 -0x1f))
($check (= 32767 #x7fff))
($check (= -255 #x-ff))

;; Signed decimal literals
($check (= 1 +1))
($check (= -1 -1))
($check (= 3.14 +3.14))

;; expt and sqrt edge cases
($check (= 0.5 (expt 2 -1)))
($check (= 0.25 (expt 2 -2)))
($check (= 1.4142135623730951 (expt 2 0.5)))
($check (= 1 (expt 0 0)))
($check (= 0 (expt 0 3)))
($check (= -8 (expt -2 3)))
($check (= 4 (expt -2 2)))
($check (= 2 (sqrt 4)))
($check (= 0 (sqrt 0)))
($check (= 1.5 (sqrt 2.25)))
($check (= 2.0000000000000004 (expt (sqrt 2) 2)))

;; division by zero gives an infinity, which is a number and compares
($check (> (/ 1 0) 0))
($check (< (- 0 (/ 1 0)) 0))
($check (number? (/ 1 0)))
($check (< 1 (/ 1 0)))
($check (> 1 (- 0 (/ 1 0))))
($check (= (/ 1 0) (/ 2 0)))
($check (not (= (/ 1 0) (- 0 (/ 1 0)))))
($check (= (/ 1 0) (+ 1 (/ 1 0))))

;; integer? and exact? answer for every integral value
($check (integer? 2))
($check (integer? 2.0))
($check (integer? -7))
($check (not (integer? 2.5)))
($check (exact? 2))
($check (not (exact? 2.5)))

;; Sign and parity predicates
($check (positive? 2))
($check (not (positive? 0)))
($check (not (positive? -2)))
($check (negative? -2))
($check (not (negative? 0)))
($check (even? 0))
($check (even? -4))
($check (not (even? 3)))
($check (odd? -3))
($check (not (odd? 4)))

;; Rounding family
($check (= 3 (ceiling 2.1)))
($check (= -2 (ceiling -2.1)))
($check (= 2 (truncate 2.7)))
($check (= -2 (truncate -2.7)))

(for-each
  (lambda (sample)
    (let ((value (car sample)) (expected (car (cdr sample))))
      ($check (eq? expected (truncate value)))
      ($check (eq? expected (apply truncate (list value))))))
  (list
    (list 0 0)
    (list -0.0 0)
    (list 5e-324 0)
    (list -5e-324 0)
    (list 0.9999999999999999 0)
    (list -0.9999999999999999 0)
    (list 1.0000000000000002 1)
    (list -1.0000000000000002 -1)
    (list 4503599627370495.5 4503599627370495)
    (list -4503599627370495.5 -4503599627370495)
    (list 4503599627370496 4503599627370496)
    (list -4503599627370496 -4503599627370496)
    (list 1.7976931348623157e308 1.7976931348623157e308)
    (list -1.7976931348623157e308 -1.7976931348623157e308)
    (list (/ 1 0) (/ 1 0))
    (list (/ -1 0) (/ -1 0))
    (list (/ 0 0) (/ 0 0))))

(let loop ((whole 1))
  (when (< whole 4503599627370496)
    (let ((fraction (+ whole 0.5)))
      ($check (eq? whole (truncate whole)))
      ($check (eq? (- whole) (truncate (- whole))))
      ($check (eq? whole (truncate fraction)))
      ($check (eq? (- whole) (truncate (- fraction))))
      ($check (eq? whole (apply truncate (list fraction))))
      ($check (eq? (- whole) (apply truncate (list (- fraction))))))
    (loop (* whole 2))))

(define truncate-alias truncate)
(define (truncate-tail value)
  (truncate value))
(define (truncate-captured value)
  (lambda () (truncate value)))

($check (= 12 (truncate-tail 12.75)))
($check (= -12 (truncate-tail -12.75)))
($check (= 12 ((truncate-captured 12.75))))
($check (= -12 (truncate-alias -12.75)))
($check (= 12 ((%prim "truncate") 12.75)))
($check (= -12 (apply truncate (list -12.75))))
($check (= 12 (let ((value 12.75)) (set! value (truncate value)) value)))
($check (= 25 (+ (truncate 12.75) (truncate 13.25))))
($check (= 12 (truncate (let ((value 12.25)) (+ value 0.5)))))

($check (= 99 (let ((truncate (lambda (value) 99))) (truncate 12.75))))
($check (= 99 (let ((truncate (%prim "truncate")))
               (set! truncate (lambda (value) 99))
               (truncate 12.75))))
($check (= 99 (let ((truncate (%prim "truncate")))
               (define (call value) (truncate value))
               (set! truncate (lambda (value) 99))
               (call 12.75))))
($check (= 12 (truncate 12.75)))

($check (= 2 (round 2.4)))
($check (= 3 (round 2.5)))
($check (= -3 (round -2.5)))
($check (= 9 (square -3)))

;; Trigonometry and exponentials at exact points
($check (= 0 (sin 0)))
($check (= 1 (cos 0)))
($check (= 0 (tan 0)))
($check (= 0 (asin 0)))
($check (= 0 (acos 1)))
($check (= 0 (atan 0)))
($check (= 1 (exp 0)))
($check (= 0 (log 1)))

;; Fold identities and unary forms
($check (= 0 (+)))
($check (= 1 (*)))
($check (= 0.5 (/ 2)))
($check (= 7 (- 10 1 2)))
($check (= 4 (/ 24 2 3)))
($check (= 3.5 (max 1 3.5 2)))
($check (= -1 (min 3 -1 2)))

;; random returns a non-negative integer below 2^31
($check (integer? (random)))
($check (<= 0 (random)))
($check (< (random) 2147483648))

;; Chained comparisons use overlapping pairs
($check (= 2 2 2))
($check (not (= 2 2 3)))
($check (< 1 2 3))
($check (not (< 1 3 2)))
($check (not (< 1 2 2)))
($check (<= 1 2 2))
($check (> 3 2 1))
($check (not (> 3 1 2)))
($check (>= 3 3 1))
($check (= 1 1 1 1 1))

;; expt edge cases
($check (= 1 (expt 0 0)))
($check (= 0 (expt 0 5)))
($check (= 0.25 (expt 2 -2)))
($check (= 8 (expt 2 3)))
($check (= 3 (expt 9 0.5)))
($check (= 0.001 (expt 10 -3)))

;; sqrt of a negative number is the canonical NaN; NaN propagates and is unordered
($check (eq? (sqrt -1) (/ 0 0)))
($check (eq? (+ (sqrt -1) 1) (/ 0 0)))
($check (not (< (sqrt -1) 0)))
($check (not (> (sqrt -1) 0)))
($check (not (<= (sqrt -1) (sqrt -1))))

;; random-seed reseeds and random keeps working
($check (begin (random-seed) #t))
($check (integer? (random)))

;; numeric-tower predicates all mean "number"
($check (real? 1.5))
($check (rational? 1.5))
($check (complex? 1.5))
($check (not (real? 'a)))
($check (not (rational? "1")))
($check (not (complex? #t)))

($check (string=? (number->string 0) "0"))
($check (string=? (number->string 999999) "999999"))
($check (string=? (number->string 1000000) "1000000"))
($check (string=? (number->string -1000000) "-1000000"))
($check (string=? (number->string 1500000) "1500000"))
($check (string=? (number->string 2000000) "2000000"))
($check (string=? (number->string 10000000) "10000000"))
($check (string=? (number->string 1e20) "100000000000000000000"))
($check (string=? (number->string 1e21) "1e+21"))
($check (string=? (number->string 0.5) "0.5"))
($check (string=? (number->string 1e-7) "1e-07"))

(define infinity (/ 1 0))
(define nan (/ 0 0))
(define values (list 0 -0.0 1 -1 3.5 -3.5 5e-324 1.7976931348623157e308 infinity (- infinity) nan))

(for-each
  (lambda (lhs)
    (for-each
      (lambda (rhs)
        ($check (eq? (apply min (list lhs rhs)) (min lhs rhs)))
        ($check (eq? (apply max (list lhs rhs)) (max lhs rhs))))
      values)
    ($check (eq? (apply min (list lhs 0)) (min lhs 0)))
    ($check (eq? (apply max (list lhs 0)) (max lhs 0)))
    ($check (eq? (apply min (list 0 lhs)) (min 0 lhs)))
    ($check (eq? (apply max (list 0 lhs)) (max 0 lhs))))
  values)

($check (eq? nan (min nan 1)))
($check (eq? nan (max nan 1)))
($check (eq? 1 (min 1 nan)))
($check (eq? 1 (max 1 nan)))
($check (eq? 0 (min 0 -0.0)))
($check (eq? 0 (max -0.0 0)))
($check (= 3 (min 3)))
($check (= 3 (max 3)))
($check (= -2 (min 4 -2 7)))
($check (= 7 (max 4 -2 7)))

(let ((min +) (max -))
  ($check (= 9 (min 4 5)))
  ($check (= -1 (max 4 5))))

(let ((min min) (max max))
  ($check (= 4 (min 4 5)))
  ($check (= 5 (max 4 5)))
  (set! min +)
  (set! max -)
  ($check (= 9 (min 4 5)))
  ($check (= -1 (max 4 5))))

(let ((order '()))
  ($check (= 2 (min (begin (set! order (cons 'left order)) 2)
                    (begin (set! order (cons 'right order)) 3))))
  ($check (equal? '(right left) order)))
