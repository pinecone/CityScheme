;; SPDX-License-Identifier: MIT
;; Copyright (c) 2026 Kirill Zorin

;;; pairs and lists
(define cons (%prim "cons"))
(define car (%prim "car"))
(define cdr (%prim "cdr"))
(define set-car! (%prim "set-car!"))
(define set-cdr! (%prim "set-cdr!"))
(define pair? (%prim "pair?"))
(define null? (%prim "null?"))
(define list? (%prim "list?"))
(define list (lambda x x))

(define (caar x) (car (car x)))
(define (cadr x) (car (cdr x)))
(define (cdar x) (cdr (car x)))
(define (cddr x) (cdr (cdr x)))
(define (caddr x) (car (cddr x)))
(define (cdddr x) (cdr (cddr x)))
(define (cadddr x) (car (cdddr x)))
(define (cddddr x) (cdr (cdddr x)))

(define first car)
(define second cadr)
(define third caddr)
(define fourth cadddr)
(define (fifth x) (car (cddddr x)))
(define rest cdr)

(define (take x i)
  (if (zero? i)
      '()
      (cons (car x) (take (cdr x) (- i 1)))))

(define (drop x i)
  (if (zero? i)
      x
      (drop (cdr x) (- i 1))))

(define (last x)
  (if (null? (cdr x))
      (car x)
      (last (cdr x))))

(define (length x)
  (let loop ((x x) (n 0))
    (if (null? x)
        n
        (loop (cdr x) (+ n 1)))))

(define append (%prim "append"))

(define (concatenate list-of-lists)
  (reduce-right append '() list-of-lists))

(define (reverse ls)
  (fold (lambda (x ls) (cons x ls)) '() ls))

(define (fold f init lst)
  (if (null? lst)
      init
      (fold f (f (car lst) init) (cdr lst))))

(define (fold-right kons knil lis)
  (if (null? lis)
      knil
      (kons (car lis) (fold-right kons knil (cdr lis)))))

(define (reduce-right f ridentity list)
  (if (null? list)
      ridentity
      (if (null? (cdr list))
          (car list)
          (f (car list) (reduce-right f ridentity (cdr list))))))

(define (for-each f lst)
  (if (not (null? lst))
      (begin
        (f (car lst))
        (for-each f (cdr lst)))))

(define (map f lst)
  (if (null? lst)
      '()
      (cons (f (car lst)) (map f (cdr lst)))))

(define (filter f l)
  (if (null? l)
      '()
      (let ((head (car l))
            (tail (cdr l)))
        (if (f head)
            (cons head (filter f tail))
            (filter f tail)))))

(define (assq k a)
  (if (null? a) #f
      (if (eq? k (car (car a))) (car a) (assq k (cdr a)))))

(define (assv k a)
  (if (null? a) #f
      (if (eqv? k (car (car a))) (car a) (assv k (cdr a)))))

(define (assoc k a)
  (if (null? a) #f
      (if (equal? k (car (car a))) (car a) (assoc k (cdr a)))))

;;; booleans
(define (not x) (if x #f #t))
(define (zero? x) (= x 0))
(define boolean? (%prim "boolean?"))
(define boolean=? (%prim "boolean=?"))

;;; i/o
(define displayn (lambda args (apply display args) (apply newline (cdr args))))
(define newline (lambda ports (apply display (cons "\n" ports))))

(define error (%prim "error"))
(define exit (%prim "exit"))
(define argv (%prim "argv"))

(define read (%prim "read"))
(define display (%prim "display"))
(define write (%prim "write"))

;;; equality
(define eqv? (%prim "eqv?"))
(define eq? (%prim "eq?"))
(define equal? (%prim "equal?"))

;;; arithmetic
(define + (%prim "+"))
(define - (%prim "-"))
(define * (%prim "*"))
(define / (%prim "/"))
(define floor (%prim "floor"))
(define ceiling (%prim "ceiling"))
(define truncate (%prim "truncate"))
(define round (%prim "round"))
(define sqrt (%prim "sqrt"))
(define expt (%prim "expt"))
(define abs (%prim "abs"))
(define exp (%prim "exp"))
(define log (%prim "log"))
(define sin (%prim "sin"))
(define cos (%prim "cos"))
(define tan (%prim "tan"))
(define asin (%prim "asin"))
(define acos (%prim "acos"))
(define atan (%prim "atan"))
(define square (%prim "square"))
(define quotient (%prim "quotient"))
(define remainder (%prim "remainder"))
(define positive? (%prim "positive?"))
(define negative? (%prim "negative?"))
(define even? (%prim "even?"))
(define odd? (%prim "odd?"))
(define number? (%prim "number?"))
(define integer? (%prim "integer?"))
(define exact? (%prim "exact?"))
(define real? (%prim "real?"))
(define rational? (%prim "rational?"))
(define complex? (%prim "complex?"))
(define = (%prim "="))
(define < (%prim "<"))
(define <= (%prim "<="))
(define > (%prim ">"))
(define >= (%prim ">="))
(define modulo (%prim "modulo"))
(define max (%prim "max"))
(define min (%prim "min"))
(define random (%prim "random"))
(define random-seed (%prim "random-seed"))
(define time-monotonic (%prim "time-monotonic"))

;;; bit ops
(define bitwise-and (%prim "bitwise-and"))
(define bitwise-ior (%prim "bitwise-ior"))
(define bitwise-xor (%prim "bitwise-xor"))
(define bitwise-not (%prim "bitwise-not"))
(define arithmetic-shift (%prim "arithmetic-shift"))

;;; symbols
(define symbol? (%prim "symbol?"))
(define symbol->string (%prim "symbol->string"))
(define string->symbol (%prim "string->symbol"))
(define symbol=? (%prim "symbol=?"))

;;; ports
(define open-input-file (%prim "open-input-file"))
(define open-input-file/maybe (%prim "open-input-file/maybe"))
(define open-output-file (%prim "open-output-file"))
(define close-input-port (%prim "close-input-port"))
(define close-output-port (%prim "close-output-port"))
(define read-char (%prim "read-char"))
(define read-bytes/all (%prim "read-bytes/all"))
(define write-bytes (%prim "write-bytes"))
(define write-char (%prim "write-char"))
(define input-port? (%prim "input-port?"))
(define output-port? (%prim "output-port?"))
(define eof-object? (%prim "eof-object?"))

;;; vectors
(define %iter (%prim "%iter"))
(define vector (%prim "vector"))
(define make-vector (%prim "make-vector"))
(define (vector-ref v i) (ref v i))
(define (vector-set! v i x) (setf! v i x))
(define vector-length (%prim "vector-length"))
(define vector? (%prim "vector?"))
(define vector-push! (%prim "vector-push!"))
(define vector-pop! (%prim "vector-pop!"))
(define vector-pop-first! (%prim "vector-pop-first!"))

;;; bytevectors
(define bytevector (%prim "bytevector"))
(define make-bytevector (%prim "make-bytevector"))
(define (bytevector-u8-ref bv i) (ref bv i))
(define (bytevector-u8-set! bv i x) (setf! bv i x))
(define bytevector-length (%prim "bytevector-length"))
(define bytevector? (%prim "bytevector?"))
(define bytevector-copy (%prim "bytevector-copy"))
(define bytevector-copy! (%prim "bytevector-copy!"))
(define bytevector-append (%prim "bytevector-append"))

;;; strings
(define string? (%prim "string?"))
(define string-append (%prim "string-append"))
(define make-string (%prim "make-string"))
(define string (%prim "string"))
(define string-length (%prim "string-length"))
(define (string-ref s i) (ref s i))
(define substring (%prim "substring"))
(define string-copy (%prim "string-copy"))
(define string=? (%prim "string=?"))
(define string<? (%prim "string<?"))
(define string<=? (%prim "string<=?"))
(define string>? (%prim "string>?"))
(define string>=? (%prim "string>=?"))
(define string->number (%prim "string->number"))
(define ascii-downcase (%prim "ascii-downcase"))
(define number->string (%prim "number->string"))

;;; characters
(define char? (%prim "char?"))
(define char->integer (%prim "char->integer"))
(define integer->char (%prim "integer->char"))
(define char=? (%prim "char=?"))
(define char<? (%prim "char<?"))
(define char<=? (%prim "char<=?"))
(define char>? (%prim "char>?"))
(define char>=? (%prim "char>=?"))
(define char-ci=? (%prim "char-ci=?"))
(define char-ci<? (%prim "char-ci<?"))
(define char-ci<=? (%prim "char-ci<=?"))
(define char-ci>? (%prim "char-ci>?"))
(define char-ci>=? (%prim "char-ci>=?"))
(define char-alphabetic? (%prim "char-alphabetic?"))
(define char-numeric? (%prim "char-numeric?"))
(define char-whitespace? (%prim "char-whitespace?"))
(define char-upper-case? (%prim "char-upper-case?"))
(define char-lower-case? (%prim "char-lower-case?"))
(define char-upcase (%prim "char-upcase"))
(define char-downcase (%prim "char-downcase"))
(define digit-value (%prim "digit-value"))

;;; procedures
(define procedure? (%prim "procedure?"))

;;; polymorphic field access
(define ref (%prim "ref"))

;;; structs
(define struct (%prim "struct"))
(define isa? (%prim "isa?"))
(define tuple (%prim "tuple"))

;;; hash tables
(define hashset (%prim "hashset"))
(define hashmap (%prim "hashmap"))
(define hashset? (%prim "hashset?"))
(define hashmap? (%prim "hashmap?"))
(define coroutine? (%prim "coroutine?"))
(define (coro/next c default) (%if/next! ((v) c) v default))
(define hashset-length (%prim "hashset-length"))
(define hashset-unset! (%prim "hashset-unset!"))
(define hashmap-unset! (%prim "hashmap-unset!"))

;;; test harness
(define %check (%prim "%check"))
