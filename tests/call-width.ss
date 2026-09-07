;; Fixed-arity calls at 15, 16, and 17 arguments cover both sides of the fast-entry split.

(define (sum15 a b c d e f g h i j k l m n o)
  (+ a b c d e f g h i j k l m n o))
(define (sum16 a b c d e f g h i j k l m n o p)
  (+ a b c d e f g h i j k l m n o p))
(define (sum17 a b c d e f g h i j k l m n o p q)
  (+ a b c d e f g h i j k l m n o p q))

($check (= 120 (sum15 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15)))
($check (= 136 (sum16 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16)))
($check (= 153 (sum17 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17)))

;; The same widths in tail position
(define (tail15 x) (sum15 x x x x x x x x x x x x x x x))
(define (tail16 x) (sum16 x x x x x x x x x x x x x x x x))
(define (tail17 x) (sum17 x x x x x x x x x x x x x x x x x))

($check (= 15 (tail15 1)))
($check (= 32 (tail16 2)))
($check (= 51 (tail17 3)))

;; Tail self-call at 17 arguments loops through the slow path
(define (count17 a b c d e f g h i j k l m n o p q)
  (if (= a 0)
      q
      (count17 (- a 1) b c d e f g h i j k l m n o p (+ q 1))))
($check (= 1000 (count17 1000 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0)))

;; Variadic entry with rest-list length 0, 1, and many
(define rest-len (lambda args (length (cdr args))))
($check (= 0 (rest-len 'x)))
($check (= 1 (rest-len 'x 1)))
($check (= 5 (rest-len 'x 1 2 3 4 5)))

(define all-args (lambda args args))
($check (equal? '() (all-args)))
($check (equal? '(1) (all-args 1)))
($check (= 20 (length (all-args 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20))))

;; Variadic tail self-call
(define drain
  (lambda xs
    (if (null? xs)
        'done
        (apply drain (cdr xs)))))
($check (eq? 'done (drain 1 2 3 4 5)))

(define (collect24 a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24)
  (list a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24))
(define (collect25 a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24 a25)
  (list a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24 a25))
(define (collect32 a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24 a25 a26 a27 a28 a29 a30 a31 a32)
  (list a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24 a25 a26 a27 a28 a29 a30 a31 a32))
(define (collect33 a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24 a25 a26 a27 a28 a29 a30 a31 a32 a33)
  (list a1 a2 a3 a4 a5 a6 a7 a8 a9 a10 a11 a12 a13 a14 a15 a16 a17 a18 a19 a20 a21 a22 a23 a24 a25 a26 a27 a28 a29 a30 a31 a32 a33))

(define (tail24 procedure)
  (procedure 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1))
(define (tail25 procedure)
  (procedure 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1))
(define (tail32 procedure)
  (procedure 32 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1))
(define (tail33 procedure)
  (procedure 33 32 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1))

(define (descending count)
  (if (= count 0) '() (cons count (descending (- count 1)))))

(let loop ((remaining 10))
  (when (> remaining 0)
    ($check (equal? (descending 24) (tail24 collect24)))
    ($check (equal? (descending 25) (tail25 collect25)))
    ($check (equal? (descending 32) (tail32 collect32)))
    ($check (equal? (descending 33) (tail33 collect33)))
    ($check (equal? (descending 24) (apply collect24 (descending 24))))
    ($check (equal? (descending 25) (apply collect25 (descending 25))))
    ($check (equal? (descending 32) (apply collect32 (descending 32))))
    ($check (equal? (descending 33) (apply collect33 (descending 33))))
    (loop (- remaining 1))))
