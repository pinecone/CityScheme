;; Lambda lifting: letrec/named-let lambdas whose captures become parameters,
;; and closure capture semantics.

;; basic shape: one immutable capture
(define (run limit offset)
  (letrec ((loop (lambda (n i)
                   (if (< i n) (loop n (+ i offset)) i))))
    (loop limit 0)))
($check (= 10 (run 10 5)))

;; two captures, deep self-recursion
(define (gauss lim a b)
  (letrec ((go (lambda (i acc)
                 (if (< i lim) (go (+ i 1) (+ acc (+ a b))) acc))))
    (go 0 0)))
($check (= 300000 (gauss 100000 1 2)))

;; nested named lets: the inner lift's rewritten call sites feed the outer lift
(define (fill out xs)
  (let loop ((todo xs) (acc '()))
    (if (null? todo)
        acc
        (let ((v (car todo)))
          (let ((next (let walk ((c v) (a acc))
                        (if (< c out) (walk (+ c 1) (cons c a)) a))))
            (loop (cdr todo) next))))))
($check (equal? '(2 2 1) (fill 3 '(1 2))))

;; set! of a capture inside the loops: writes must reach the shared binding
(define (count-pairs groups)
  (let ((count 0))
    (for-each
      (lambda (reduced)
        (let outer ((xs reduced))
          (when (pair? xs)
            (let ((m1 (car xs)))
              (let inner ((ys (cdr xs)))
                (when (pair? ys)
                  (when (eq? m1 (car ys))
                    (set! count (+ count 1)))
                  (inner (cdr ys))))
              (outer (cdr xs))))))
      groups)
    count))
($check (= 4 (count-pairs (list (list 1 2 1 1) (list 3 3)))))

;; the lambda escapes as a value from its own body: arity must not change
(define (make-counter step)
  (letrec ((tick (lambda (n)
                   (if (< n 1) tick (tick (- n step))))))
    (tick 10)))
($check (procedure? (((make-counter 2) 0) 5)))

;; the lambda escapes as a value from the let body
(define (pick off)
  (letrec ((g (lambda (x) (+ x off))))
    (map g '(1 2 3))))
($check (equal? '(11 12 13) (pick 10)))

;; non-tail self call
(define (sum-to lim add-on)
  (letrec ((go (lambda (i)
                 (if (< i lim) (+ add-on (go (+ i 1))) 0))))
    (go 0)))
($check (= 50 (sum-to 5 10)))
;; A second invocation executes the same cds site with a new self closure.
($check (= 21 (sum-to 3 7)))

;; binding reassigned after init
(define (twice off)
  (letrec ((f (lambda (x) (+ x off))))
    (set! f (lambda (x) (* x off)))
    (f 3)))
($check (= 12 (twice 4)))

;; nested lambda inside the loop capturing the loop's own state
(define (tab off n)
  (letrec ((build (lambda (i acc)
                    (if (< i n)
                        (build (+ i 1) (cons (lambda () (+ i off)) acc))
                        acc))))
    (map (lambda (f) (f)) (build 0 '()))))
($check (equal? '(102 101 100) (tab 100 3)))

;; Inline-applied lambda with an internal define.
($check (= 10 ((lambda (x) (define y (* x 2)) y) 5)))
;; Inline-applied variadic lambda.
($check (equal? '(1 2 3) ((lambda args args) 1 2 3)))
($check (= 10 ((lambda args (apply + args)) 1 2 3 4)))

;; set! on own param (no capture).
($check (= 42 ((lambda (x) (set! x 42) x) 1)))
(define (mut x) (set! x (* x 2)) x)
($check (= 10 (mut 5)))

;; set! on a captured variable: each counter keeps its own state across calls.
(define (make-c init)
  (lambda ()
    (set! init (+ init 1))
    init))
(define c1 (make-c 0))
($check (= 1 (c1)))
($check (= 2 (c1)))
($check (= 3 (c1)))
;; Independent counters share no state.
(define c2 (make-c 100))
($check (= 101 (c2)))
($check (= 4 (c1)))
($check (= 102 (c2)))

;; Closed inner lambda passed to map.
(define (squares-of lst) (map (lambda (x) (* x x)) lst))
($check (equal? '(1 4 9 16) (squares-of '(1 2 3 4))))

;; apply on a closure that captures.
(define (cons-x x) (lambda args (cons x args)))
($check (equal? '(a 1 2) (apply (cons-x 'a) '(1 2))))

;; Computed callee (not a direct symbol reference).
(define (pick-op t) (if t + -))
($check (= 8 ((pick-op #t) 5 3)))
($check (= 2 ((pick-op #f) 5 3)))

(define (accumulate limit step)
  (let ((total 0))
    (let loop ((index 0))
      (when (< index limit)
        (set! total (+ total step))
        (loop (+ index 1))))
    total))
($check (= 30 (accumulate 10 3)))

(define (observe limit step)
  (let ((value 0))
    (let ((advance (lambda () (set! value (+ value step)))))
      (let loop ((index 0) (total 0))
        (if (< index limit)
            (begin
              (advance)
              (loop (+ index 1) (+ total value)))
            total)))))
($check (= 30 (observe 5 2)))

(define (retain limit step)
  (let ((value 0))
    (let loop ((index 0) (readers '()))
      (if (< index limit)
          (begin
            (set! value (+ value step))
            (loop (+ index 1) (cons (lambda () (+ value index step)) readers)))
          (map (lambda (reader) (reader)) readers)))))
($check (equal? '(14 13 12) (retain 3 3)))

(define (initialize step)
  (letrec ((run (lambda (count)
                  (let loop ((index count) (total 0))
                    (if (> index 0)
                        (loop (- index 1) (+ total value step))
                        total))))
           (value 7))
    (run 3)))
($check (= 27 (initialize 2)))
