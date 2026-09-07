;; Let expressions

;; Basic let
(define r1 (let ((x 1) (y 2)) (+ x y)))
($check (= 3 r1))

;; Nested let
(define r2 (let ((x 1))
             (let ((y 2) (z 3))
               (+ x y z))))
($check (= 6 r2))

;; Let bindings are parallel -- y sees outer x
(define x 10)
(define r3 (let ((y x)) (+ 1 y)))
($check (= 11 r3))

;; Let with body sequence
(define r4 (let ((a 1))
             (define b 2)
             (+ a b)))
($check (= 3 r4))

;; Shadowing
(define z 100)
(define r5 (let ((z 5)) z))
($check (= 5 r5))
($check (= 100 z))

;; Inner let val is evaluated in the enclosing scope, before the inner name binds.
($check (= 25 (let ((a 5)) (let ((b (* a a))) b))))
($check (= 40 (let ((a 10)) (let ((a 20)) (+ a a)))))

;; A let-bound name that shadows a primitive wins
($check (= 42 (let ((ref (lambda (v i) 42))) (ref (vector 7 8) 0))))
($check (= 12 (let ((+ (lambda (a b) (* a b)))) (+ 3 4))))
($check (eq? 'shadowed (let ((< (lambda (a b) 'shadowed))) (< 1 2))))

;; A let-bound name that is the primitive still lowers.
($check (= 8 (let ((ref (%prim "ref"))) (ref (vector 5 8) 1))))

;; let-bound variable captured and mutated by a closure -> boxed frame slot.
(define (make-counter)
  (let ((n 0))
    (lambda () (set! n (+ n 1)) n)))
(define ctr (make-counter))
($check (= 1 (ctr)))
($check (= 2 (ctr)))
($check (= 3 (ctr)))

;; let* binds sequentially: each val sees the previous names.
($check (= 6 (let* ((a 1) (b (+ a 1)) (c (+ b 1))) (+ a b c))))
($check (= 5 (let* ((a 2) (a (+ a a)) (a (+ a 1))) a)))

;; Letrec.

;; Single self-recursion.
(define r1
  (letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1)))))))
    (fact 6)))
($check (= 720 r1))

;; Mutual recursion.
(define r2
  (letrec ((even? (lambda (n) (if (= n 0) #t (odd?  (- n 1)))))
           (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
    (even? 10)))
($check (eq? #t r2))

(define r3
  (letrec ((even? (lambda (n) (if (= n 0) #t (odd?  (- n 1)))))
           (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
    (odd? 11)))
($check (eq? #t r3))

;; letrec* keyword is accepted and behaves the same.
(define r4
  (letrec* ((a 1)
            (b (+ a 1))
            (c (+ b 1)))
    (+ a b c)))
($check (= 6 r4))

;; Body sees outer bindings normally.
(define outer 100)
(define r5
  (letrec ((f (lambda () outer)))
    (f)))
($check (= 100 r5))

;; Tail calls inside letrec'd procedures don't blow the stack.
(define r6
  (letrec ((loop (lambda (n) (if (= n 0) 'done (loop (- n 1))))))
    (loop 100000)))
($check (eq? 'done r6))

;; Named let.

;; Classic counting loop.
(define r1
  (let loop ((i 0) (sum 0))
    (if (= i 10) sum (loop (+ i 1) (+ sum i)))))
($check (= 45 r1))

;; Building a list, accumulator-style.
(define r2
  (let build ((n 5) (acc '()))
    (if (= n 0) acc (build (- n 1) (cons n acc)))))
($check (equal? '(1 2 3 4 5) r2))

;; Deep tail recursion through the named-let loop.
(define r3
  (let go ((n 100000))
    (if (= n 0) 'done (go (- n 1)))))
($check (eq? 'done r3))

;; Body that closes over outer lexicals.
(define limit 7)
(define r4
  (let walk ((i 0))
    (if (= i limit) i (walk (+ i 1)))))
($check (= 7 r4))

;; Named let returning a closure that captures a loop param.
(define adder
  (let make ((n 5))
    (lambda (x) (+ x n))))
($check (= 12 (adder 7)))

(define (write-copy value)
  (let ((copy value))
    (set! copy 5)
    (list value copy)))
($check (equal? '(10 5) (write-copy 10)))

(define (write-source value)
  (let ((copy value))
    (set! value 99)
    (list value copy)))
($check (equal? '(99 10) (write-source 10)))

(define (shadow value)
  (let ((value value))
    (set! value 5)
    ($check (= 5 value)))
  value)
($check (= 10 (shadow 10)))

(define (chain value)
  (let* ((first value) (second first))
    (set! first 20)
    (set! value 30)
    (list value first second)))
($check (equal? '(30 20 10) (chain 10)))

(let ((outer outer))
  (set! outer 20)
  ($check (= 20 outer)))
($check (= 100 outer))

(let ((min min))
  (set! min +)
  ($check (= 5 (min 2 3))))
($check (= 2 (min 2 3)))

(define (snapshot value)
  (let ((copy value))
    (let ((read (lambda () copy)))
      (set! value 20)
      (read))))
($check (= 10 (snapshot 10)))

(define (siblings value)
  (let ((copy value))
    (let ((read (lambda () value))
          (write (lambda () (set! copy 20))))
      (write)
      (list (read) copy))))
($check (equal? '(10 20) (siblings 10)))

(define (iterate value)
  (let loop ((remaining 5) (total 0))
    (if (= remaining 0)
        (list value total)
        (let ((copy value))
          (set! copy (+ copy remaining))
          (loop (- remaining 1) (+ total copy))))))
($check (equal? '(10 65) (iterate 10)))
