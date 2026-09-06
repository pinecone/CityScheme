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
