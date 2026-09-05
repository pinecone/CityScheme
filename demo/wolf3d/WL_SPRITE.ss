;;; static objects and sprite scaling

(define SPR_STAT_0 2)
(define MAXSTATS 400)

(define dressing 0)
(define block 1)
(define bo_gibs 2)
(define bo_alpo 3)
(define bo_firstaid 4)
(define bo_key1 5)
(define bo_key2 6)
(define bo_key3 7)
(define bo_key4 8)
(define bo_cross 9)
(define bo_chalice 10)
(define bo_bible 11)
(define bo_crown 12)
(define bo_clip 13)
(define bo_clip2 14)
(define bo_machinegun 15)
(define bo_chaingun 16)
(define bo_food 17)
(define bo_fullheal 18)
(define bo_25clip 19)
(define bo_spear 20)

(define FL_BONUS 2)

(define statinfo-pic
  (vector 0 1 2 3 4 5 6 7
          8 9 10 11 12 13 14 15
          16 17 18 19 20 21 22 23
          24 25 26 27 28 29 30 31
          32 33 34 35 36 37 38 39
          40 41 42 43 44 45 46 47
          26))

(define statinfo-type
  (vector dressing block block block dressing block bo_alpo block
          block dressing block block block block dressing dressing
          block block block dressing bo_key1 bo_key2 block dressing
          bo_food bo_firstaid bo_clip bo_machinegun bo_chaingun bo_cross bo_chalice bo_bible
          bo_crown bo_fullheal bo_gibs block block block bo_gibs block
          block dressing dressing dressing dressing block block dressing
          bo_clip2))

(define spotvis (make-bytevector (* MAPSIZE MAPSIZE) 0))
(define statcount 0)
(define treasurecount 0)
(define treasuretotal 0)
(define stat-tilex (make-vector MAXSTATS 0))
(define stat-tiley (make-vector MAXSTATS 0))
(define stat-shape (make-vector MAXSTATS 0))
(define stat-flags (make-vector MAXSTATS 0))
(define stat-item (make-vector MAXSTATS 0))

(define (spotvis-clear)
  (let loop ((index 0))
    (when (< index (* MAPSIZE MAPSIZE))
      (setf! spotvis index 0)
      (loop (+ index 1)))))

(define (spotvis-mark x y) (setf! spotvis (+ (* x MAPSIZE) y) 1))
(define (spotvis-at x y) (ref spotvis (+ (* x MAPSIZE) y)))

(define (treasure-item item)
  (or (= item bo_cross) (= item bo_chalice) (= item bo_bible)
      (= item bo_crown) (= item bo_fullheal)))

(define sprite-pixels 0)

(define (column-width rows sourcex)
  (- (truncate (/ (* (+ sourcex 1) rows) 64))
     (truncate (/ (* sourcex rows) 64))))

(define (shape-column page leftpix sourcex screenx rows top level emission)
  (let posts ((offset (readu16 page (+ 4 (* (- sourcex leftpix) 2)))))
    (let ((endpixel (readu16 page offset)))
      (when (> endpixel 0)
        (let ((topofs (readi16 page (+ offset 2)))
              (startpixel (readu16 page (+ offset 4))))
          (let texels ((texel (truncate (/ startpixel 2))))
            (when (< texel (truncate (/ endpixel 2)))
              (let fill ((y (+ top (truncate (/ (* texel rows) 64))))
                         (limit (+ top (truncate (/ (* (+ texel 1) rows) 64)))))
                (when (< y limit)
                  (when (and (>= y 0) (< y viewheight))
                    (setf! framebuffer (+ (* (+ y viewtop) screenwidth) viewleft screenx)
                           (plus-sprite-color (ref page (+ topofs texel)) level emission))
                    (set! sprite-pixels (+ sprite-pixels 1)))
                  (fill (+ y 1) limit)))
              (texels (+ texel 1)))))
        (posts (+ offset 6))))))

(define (draw-run page leftpix sourcex x width rows top level emission)
  (let loop ((column 0))
    (when (< column width)
      (shape-column page leftpix sourcex (+ x column) rows top level emission)
      (loop (+ column 1)))))

(define (trim-run x width height outward-left)
  (let ((leftvis (< (ref wallheight x) height))
        (rightvis (< (ref wallheight (+ x width -1)) height)))
    (cond ((and leftvis rightvis) (list x width #f))
          (leftvis (let shrink ((w width))
                     (if (>= (ref wallheight (+ x w -1)) height)
                         (shrink (- w 1))
                         (list x w (not outward-left)))))
          (rightvis (let advance ((sx x) (w width))
                      (if (>= (ref wallheight sx) height)
                          (advance (+ sx 1) (- w 1))
                          (list sx w outward-left))))
          (else (list x 0 #f)))))

(define (place-run page leftpix sourcex x width rows top height clip outward-left level emission)
  (let ((run (if clip (trim-run x width height outward-left) (list x width #f))))
    (when (> (cadr run) 0)
      (draw-run page leftpix sourcex (car run) (cadr run) rows top level emission))
    (caddr run)))

(define (scale-shape screenx shapenum height clip tilex tiley)
  (let* ((page (PM_GetPage (+ PMSpriteStart shapenum)))
         (leftpix (readu16 page 0))
         (rightpix (readu16 page 2))
         (scale (truncate (/ height (if clip 8 2)))))
    (when (and (> scale 0) (<= scale maxscale))
      (let* ((rows (* scale 2))
             (level (if clip
                        (plus-light-level (plus-distance-level rows viewheight) tilex tiley)
                        0))
             (emission (if (and clip plus-enabled) (plus-light-emission shapenum) 0))
             (top (truncate (/ (- viewheight rows) 2))))
        (let left ((sourcex 31) (slinex screenx))
          (when (and (>= sourcex leftpix) (> slinex 0))
            (let ((width (column-width rows sourcex)))
              (if (= width 0)
                  (left (- sourcex 1) slinex)
                  (let* ((clamped (if (> slinex viewwidth) width (if (> width slinex) slinex width)))
                         (x (- slinex clamped))
                         (visible (if (> slinex viewwidth) (- viewwidth x) clamped)))
                    (if (< visible 1)
                        (left (- sourcex 1) x)
                        (unless (place-run page leftpix sourcex x visible rows top height clip #t level
                                           emission)
                          (left (- sourcex 1) x))))))))
        (let right ((sourcex (if (< leftpix 31) 31 (- leftpix 1)))
                    (slinex screenx)
                    (slinewidth 0))
          (let ((next (+ sourcex 1))
                (x (+ slinex slinewidth)))
            (when (and (<= next rightpix) (< x viewwidth))
              (let ((width (column-width rows next)))
                (cond
                  ((= width 0) (right next x 0))
                  ((and (< x 0) (<= width (- x))) (right next x width))
                  (else
                   (let* ((start (if (< x 0) 0 x))
                          (visible (cond ((< x 0) (+ width x))
                                         ((> (+ x width) viewwidth) (- viewwidth x))
                                         (else width))))
                     (unless (place-run page leftpix next start visible rows top height clip #f level
                                        emission)
                       (right next start visible)))))))))))))

(define (ScaleShape screenx shapenum height tilex tiley)
  (scale-shape screenx shapenum height #t tilex tiley))

(define (SimpleScaleShape screenx shapenum height)
  (scale-shape screenx shapenum height #f 0 0))

(define MAXVISABLE 50)

(define vis-x (make-vector MAXVISABLE 0))
(define vis-height (make-vector MAXVISABLE 0))
(define vis-shape (make-vector MAXVISABLE 0))
(define vis-tilex (make-vector MAXVISABLE 0))
(define vis-tiley (make-vector MAXVISABLE 0))
(define viscount 0)

(define (vis-add screenx height shapenum tilex tiley)
  (setf! vis-x viscount screenx)
  (setf! vis-height viscount height)
  (setf! vis-shape viscount shapenum)
  (setf! vis-tilex viscount tilex)
  (setf! vis-tiley viscount tiley)
  (when (< viscount (- MAXVISABLE 1))
    (set! viscount (+ viscount 1))))

(define (place-statics)
  (let loop ((index 0))
    (when (< index statcount)
      (let ((tilex (ref stat-tilex index))
            (tiley (ref stat-tiley index)))
        (unless (or (= (ref stat-shape index) -1) (= 0 (spotvis-at tilex tiley)))
          (let* ((transformed (TransformTile tilex tiley))
                 (screenx (car transformed))
                 (height (cadr transformed))
                 (grab (caddr transformed)))
            (if (and grab (> (bitwise-and (ref stat-flags index) FL_BONUS) 0))
                (GetBonus index)
                (unless (= height 0)
                  (vis-add screenx height (ref stat-shape index) tilex tiley))))))
      (loop (+ index 1)))))

(define (place-actors)
  (let loop ((index 0))
    (when (< index actorcount)
      (let ((shapenum (if (ref actor-state index)
                          (ref (ref actor-state index) 'shapenum)
                          0)))
        (if (or (= shapenum 0) (not (actor-visible index)))
            (setf! actor-flags index (bitwise-and (ref actor-flags index) (- 255 FL_VISABLE)))
            (begin
              (setf! actor-active index #t)
              (let* ((transformed (TransformActor index))
                     (screenx (car transformed))
                     (height (cadr transformed)))
                (when (not (= height 0))
                  (vis-add screenx height
                           (+ (if (= shapenum -1) (ref actor-temp1 index) shapenum)
                              (if (actor-rotates index) (CalcRotate index screenx) 0))
                           (ref actor-tilex index) (ref actor-tiley index))
                  (setf! actor-flags index
                         (bitwise-ior (ref actor-flags index) FL_VISABLE)))))))
      (loop (+ index 1)))))

