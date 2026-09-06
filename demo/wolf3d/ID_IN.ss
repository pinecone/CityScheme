;;; ID_IN.C — keyboard input, keyboard-only

(define sc_None 0)
(define sc_Bad 255)
(define sc_Return 28)
(define sc_Enter sc_Return)
(define sc_Escape 1)
(define sc_Space 57)
(define sc_BackSpace 14)
(define sc_Tab 15)
(define sc_Alt 56)
(define sc_Control 29)
(define sc_CapsLock 58)
(define sc_LShift 42)
(define sc_RShift 54)
(define sc_UpArrow 72)
(define sc_DownArrow 80)
(define sc_LeftArrow 75)
(define sc_RightArrow 77)
(define sc_Insert 82)
(define sc_Delete 83)
(define sc_Home 71)
(define sc_End 79)
(define sc_PgUp 73)
(define sc_PgDn 81)
(define sc_F1 59)
(define sc_F2 60)
(define sc_F3 61)
(define sc_F4 62)
(define sc_F5 63)
(define sc_F6 64)
(define sc_F7 65)
(define sc_F8 66)
(define sc_F9 67)
(define sc_F10 68)
(define sc_A 30)
(define sc_N 49)
(define sc_Y 21)
(define sc_W 17)
(define sc_S 31)
(define sc_D 32)
(define sc_I 23)
(define sc_L 38)
(define sc_O 24)
(define sc_P 25)
(define sc_B 48)
(define sc_C 46)
(define sc_E 18)
(define sc_F 33)
(define sc_G 34)
(define sc_H 35)
(define sc_M 50)
(define sc_Q 16)
(define sc_T 20)
(define sc_V 47)
(define sc_X 45)

(define key_None 0)
(define key_Return 13)
(define key_Enter key_Return)
(define key_Escape 27)
(define key_Space 32)
(define key_BackSpace 8)
(define key_Tab 9)
(define key_Delete 127)

(define ctrl_Keyboard 0)
(define ctrl_Keyboard1 0)
(define ctrl_Keyboard2 1)
(define ctrl_Joystick 2)
(define ctrl_Joystick1 2)
(define ctrl_Joystick2 3)
(define ctrl_Mouse 4)

(define motion_Left -1)
(define motion_Up -1)
(define motion_None 0)
(define motion_Right 1)
(define motion_Down 1)

(define dir_North 0)
(define dir_NorthEast 1)
(define dir_East 2)
(define dir_SouthEast 3)
(define dir_South 4)
(define dir_SouthWest 5)
(define dir_West 6)
(define dir_NorthWest 7)
(define dir_None 8)

(define MaxPlayers 4)
(define NUMSCANCODES 128)
(define JoyDefs (vector (make-vector 12 0) (make-vector 12 0)))

(define kd_button0 0)
(define kd_button1 1)
(define kd_upleft 2)
(define kd_up 3)
(define kd_upright 4)
(define kd_left 5)
(define kd_right 6)
(define kd_downleft 7)
(define kd_down 8)
(define kd_downright 9)

;; ID_IN.C:64
(define KbdDefs (vector 29 56 71 72 73 75 77 79 80 81))

;; ID_IN.C:186-190
(define DirTable
  (vector dir_NorthWest dir_North dir_NorthEast
          dir_West dir_None dir_East
          dir_SouthWest dir_South dir_SouthEast))

;; ID_IN.C:82-93
(define ASCIINames
  #u8(0 27 49 50 51 52 53 54 55 56 57 48 45 61 8 9
      113 119 101 114 116 121 117 105 111 112 91 93 13 0 97 115
      100 102 103 104 106 107 108 59 39 96 0 92 122 120 99 118
      98 110 109 44 46 47 0 42 0 32 0 0 0 0 0 0
      0 0 0 0 0 0 0 55 56 57 45 52 53 54 43 49
      50 51 48 127 0 0 0 0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0))

;; ID_IN.C:94-105
(define ShiftNames
  #u8(0 27 33 64 35 36 37 94 38 42 40 41 95 43 8 9
      81 87 69 82 84 89 85 73 79 80 123 125 13 0 65 83
      68 70 71 72 74 75 76 58 34 126 0 124 90 88 67 86
      66 78 77 60 62 63 0 42 0 32 0 0 0 0 0 0
      0 0 0 0 0 0 0 55 56 57 45 52 53 54 43 49
      50 51 48 127 0 0 0 0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
      0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0))

(define Keyboard (make-vector NUMSCANCODES #f))
(define keyboard-held (make-vector NUMSCANCODES #f))
(define Paused #f)
(define LastASCII key_None)
(define LastScan sc_None)
(define Controls (make-vector MaxPlayers ctrl_Keyboard))
(define IN_Started #f)
(define MousePresent #f)
(define JoysPresent (vector #f #f))
(define CapsLock #f)
(define CurCode 0)
(define LastCode 0)
(define INL_KeyHook #f)
(define pause-held #f)
(define btnstate (make-vector 8 #f))

;; The key name that key-down? takes for each scan code. The DOS build read the
;; scan code from port 0x60; sokol reports named keys instead.
(define scan-keys (make-vector NUMSCANCODES #f))
(define scan-names (make-vector NUMSCANCODES ""))

(define (IN_GetScanName scan)
  (if (and (>= scan 0) (< scan NUMSCANCODES))
      (ref scan-names scan)
      ""))

(define letter-scans #u8(30 48 46 32 18 33 34 35 23 36 37 38 50 49 24 25 16 19 31 20 22 47 17 45 21 44))
(define fkey-scans #u8(59 60 61 62 63 64 65 66 67 68 87 89))

(let letters ((index 0))
  (when (< index 26)
    (setf! scan-keys (ref letter-scans index)
           (string->symbol (substring "abcdefghijklmnopqrstuvwxyz" index (+ index 1))))
    (letters (+ index 1))))

(let digits ((index 0))
  (when (< index 10)
    (setf! scan-keys (+ 2 index)
           (string->symbol (substring "1234567890" index (+ index 1))))
    (digits (+ index 1))))

(let fkeys ((index 0))
  (when (< index 12)
    (setf! scan-keys (ref fkey-scans index)
           (string->symbol (string-append "f" (number->string (+ index 1)))))
    (fkeys (+ index 1))))

(for-each
 (lambda (entry) (setf! scan-keys (first entry) (second entry)))
 (list (list sc_Escape 'escape) (list sc_Space 'space) (list sc_Return 'enter)
       (list sc_Tab 'tab) (list sc_BackSpace 'backspace) (list sc_Control 'control)
       (list sc_Alt 'alt) (list sc_LShift 'shift) (list sc_RShift 'right-shift)
       (list sc_CapsLock 'caps-lock) (list sc_UpArrow 'up) (list sc_DownArrow 'down)
       (list sc_LeftArrow 'left) (list sc_RightArrow 'right) (list sc_Insert 'insert)
       (list sc_Delete 'delete) (list sc_Home 'home) (list sc_End 'end)
       (list sc_PgUp 'page-up) (list sc_PgDn 'page-down)
       (list 12 'minus) (list 13 'equal) (list 26 'left-bracket) (list 27 'right-bracket)
       (list 39 'semicolon) (list 40 'apostrophe) (list 41 'grave) (list 43 'backslash)
       (list 51 'comma) (list 52 'period) (list 53 'slash)))

(let letters ((index 0))
  (when (< index 26)
    (setf! scan-names (ref letter-scans index)
           (substring "ABCDEFGHIJKLMNOPQRSTUVWXYZ" index (+ index 1)))
    (letters (+ index 1))))

(let digits ((index 0))
  (when (< index 10)
    (setf! scan-names (+ 2 index) (substring "1234567890" index (+ index 1)))
    (digits (+ index 1))))

(let fkeys ((index 0))
  (when (< index 12)
    (setf! scan-names (ref fkey-scans index) (string-append "F" (number->string (+ index 1))))
    (fkeys (+ index 1))))

(for-each
 (lambda (entry) (setf! scan-names (first entry) (second entry)))
 (list (list sc_Escape "Esc") (list sc_BackSpace "BkSp") (list sc_Tab "Tab")
       (list sc_Control "Ctrl") (list sc_LShift "LShft") (list sc_Space "Space")
       (list sc_CapsLock "CapsLk") (list sc_Return "Enter") (list sc_RShift "RShft")
       (list sc_Alt "Alt") (list sc_Home "Home") (list sc_PgUp "PgUp")
       (list sc_End "End") (list sc_PgDn "PgDn") (list sc_Insert "Ins")
       (list sc_Delete "Del") (list sc_UpArrow "Up") (list sc_DownArrow "Down")
       (list sc_LeftArrow "Left") (list sc_RightArrow "Right")
       (list 12 "-") (list 13 "+") (list 26 "[") (list 27 "]")
       (list 39 ";") (list 40 "\"") (list 43 "|") (list 51 ",")
       (list 52 ".") (list 53 "/")))

;; ID_IN.C:198-206
(define (key-ascii code)
  (if (or (ref Keyboard sc_LShift) (ref Keyboard sc_RShift))
      (let ((c (ref ShiftNames code)))
        (if (and (>= c 65) (<= c 90) CapsLock) (+ c 32) c))
      (let ((c (ref ASCIINames code)))
        (if (and (>= c 97) (<= c 122) CapsLock) (- c 32) c))))

;; INL_KeyService (ID_IN.C:143-206) was a keyboard interrupt. There is no
;; interrupt here, so the driver calls this once per frame and the make and
;; break codes come from the difference against the last poll.
(define (IN_PollKeyboard)
  ;; The DOS handler saw scan code 0xe1 for the Pause key (ID_IN.C:158-161).
  (let ((pause (dos:key-down? 'pause)))
    (when (and pause (not pause-held))
      (set! Paused #t))
    (set! pause-held pause))
  (let scan ((code 0))
    (when (< code NUMSCANCODES)
      (let ((name (ref scan-keys code)))
        (when name
          (let ((down (dos:key-down? name)))
            (cond ((and down (not (ref keyboard-held code)))
                   (setf! keyboard-held code #t)
                   (set! LastCode CurCode)
                   (set! CurCode code)
                   (set! LastScan code)
                   (setf! Keyboard code #t)
                   (when (= code sc_CapsLock)
                     (set! CapsLock (not CapsLock)))
                   (let ((c (key-ascii code)))
                     (when (> c 0)
                       (set! LastASCII c)))
                   (when INL_KeyHook
                     (INL_KeyHook)))
                  ((and (not down) (ref keyboard-held code))
                   (setf! keyboard-held code #f)
                   (setf! Keyboard code #f)
                   (when INL_KeyHook
                     (INL_KeyHook)))))))
      (scan (+ code 1)))))

(define (INL_KeyService)
  (IN_PollKeyboard))

(define (INL_GetMouseDelta)
  (tuple (dos:get-mouse-motion-x) (dos:get-mouse-motion-y)))

(define (INL_GetMouseButtons)
  (bitwise-ior (if (dos:mouse-button-down? 'left) 1 0)
               (bitwise-ior (if (dos:mouse-button-down? 'right) 2 0)
                            (if (dos:mouse-button-down? 'middle) 4 0))))

(define (IN_MouseButtons)
  (if MousePresent (INL_GetMouseButtons) 0))

(define (IN_GetJoyAbs joy)
  (tuple 0 0))

(define (INL_GetJoyDelta joy)
  (tuple 0 0))

(define (INL_GetJoyButtons joy)
  0)

(define (IN_GetJoyButtonsDB joy)
  (INL_GetJoyButtons joy))

(define (IN_JoyButtons)
  0)

(define (INL_SetJoyScale joy)
  (let ((def (ref JoyDefs joy)))
    (setf! def 8 (quotient 32768 (- (ref def 2) (ref def 0))))
    (setf! def 10 (quotient 32768 (- (ref def 6) (ref def 4))))
    (setf! def 9 (quotient 32768 (- (ref def 3) (ref def 1))))
    (setf! def 11 (quotient 32768 (- (ref def 7) (ref def 5))))))

(define (IN_SetupJoy joy minx maxx miny maxy)
  (let ((def (ref JoyDefs joy))
        (xrange (- maxx minx))
        (yrange (- maxy miny)))
    (setf! def 0 minx)
    (setf! def 6 maxx)
    (setf! def 2 (+ (- (quotient xrange 2) (quotient xrange 3)) minx))
    (setf! def 4 (+ (quotient xrange 2) (quotient xrange 3) minx))
    (setf! def 1 miny)
    (setf! def 7 maxy)
    (setf! def 3 (+ (- (quotient yrange 2) (quotient yrange 3)) miny))
    (setf! def 5 (+ (quotient yrange 2) (quotient yrange 3) miny))
    (INL_SetJoyScale joy)))

(define (INL_StartJoy joy)
  #f)

(define (INL_ShutJoy joy)
  (setf! JoysPresent joy #f))

;; ID_IN.C:605-613
(define (IN_ClearKeysDown)
  (set! LastScan sc_None)
  (set! LastASCII key_None)
  (let clear ((code 0))
    (when (< code NUMSCANCODES)
      (setf! Keyboard code #f)
      (clear (+ code 1)))))

;; ID_IN.H:178-180
(define (IN_KeyDown code) (ref Keyboard code))

(define (IN_ClearKey code)
  (setf! Keyboard code #f)
  (when (= code LastScan)
    (set! LastScan sc_None)))

(define (INL_StartKbd)
  (set! INL_KeyHook #f)
  (IN_ClearKeysDown))

(define (INL_ShutKbd)
  #f)

(define (INL_StartMouse)
  #t)

(define (INL_ShutMouse)
  #f)

;; ID_IN.C:519-542, keyboard only
(define (IN_Startup)
  (unless IN_Started
    (INL_StartKbd)
    (let scan ((index 0) (checkmouse #t) (checkjoys #t))
      (if (= index (vector-length argv))
          (begin
            (set! MousePresent (and checkmouse (INL_StartMouse)))
            (let start-joys ((joy 0))
              (when (< joy (vector-length JoysPresent))
                (setf! JoysPresent joy (and checkjoys (INL_StartJoy joy)))
                (start-joys (+ joy 1)))))
          (let ((argument (ref argv index)))
            (scan (+ index 1)
                  (and checkmouse
                       (not (or (string=? argument "nomouse")
                                (string=? argument "-nomouse")
                                (string=? argument "/nomouse"))))
                  (and checkjoys
                       (not (or (string=? argument "nojoys")
                                (string=? argument "-nojoys")
                                (string=? argument "/nojoys"))))))))
    (set! IN_Started #t)))

;; ID_IN.C:566-578, keyboard only
(define (IN_Shutdown)
  (when IN_Started
    (INL_ShutMouse)
    (let shut-joys ((joy 0))
      (when (< joy (vector-length JoysPresent))
        (INL_ShutJoy joy)
        (shut-joys (+ joy 1))))
    (INL_ShutKbd)
    (set! IN_Started #f)))

(define (IN_SetKeyHook hook)
  (set! INL_KeyHook hook))

(define (IN_Default gotit type)
  (let ((selected
         (if (or (not gotit)
                 (and (= type ctrl_Joystick1) (not (ref JoysPresent 0)))
                 (and (= type ctrl_Joystick2) (not (ref JoysPresent 1)))
                 (and (= type ctrl_Mouse) (not MousePresent)))
             ctrl_Keyboard1
             type)))
    (IN_SetControlType 0 selected)))

;; ID_IN.C:817-824
(define (IN_SetControlType player type)
  (setf! Controls player type))

(define in-x 0)
(define in-y 0)
(define in-xaxis motion_None)
(define in-yaxis motion_None)
(define in-button0 #f)
(define in-button1 #f)
(define in-button2 #f)
(define in-button3 #f)
(define in-dir dir_None)

;; ID_IN.C:684-741, the ctrl_Keyboard case
(define (IN_ReadControl player)
  (let ((mx motion_None) (my motion_None) (dx 0) (dy 0) (buttons 0) (realdelta #f)
        (type (ref Controls player)))
    (cond
      ((or (= type ctrl_Keyboard1) (= type ctrl_Keyboard2))
       (cond ((ref Keyboard (ref KbdDefs kd_upleft)) (set! mx motion_Left) (set! my motion_Up))
             ((ref Keyboard (ref KbdDefs kd_upright)) (set! mx motion_Right) (set! my motion_Up))
             ((ref Keyboard (ref KbdDefs kd_downleft)) (set! mx motion_Left) (set! my motion_Down))
             ((ref Keyboard (ref KbdDefs kd_downright)) (set! mx motion_Right) (set! my motion_Down)))
       (cond ((ref Keyboard (ref KbdDefs kd_up)) (set! my motion_Up))
             ((ref Keyboard (ref KbdDefs kd_down)) (set! my motion_Down)))
       (cond ((ref Keyboard (ref KbdDefs kd_left)) (set! mx motion_Left))
             ((ref Keyboard (ref KbdDefs kd_right)) (set! mx motion_Right)))
       (when (ref Keyboard (ref KbdDefs kd_button0)) (set! buttons (+ buttons 1)))
       (when (ref Keyboard (ref KbdDefs kd_button1)) (set! buttons (+ buttons 2))))
      ((or (= type ctrl_Joystick1) (= type ctrl_Joystick2))
       (let ((delta (INL_GetJoyDelta (- type ctrl_Joystick))))
         (set! dx (ref delta 0))
         (set! dy (ref delta 1)))
       (set! buttons (INL_GetJoyButtons (- type ctrl_Joystick)))
       (set! realdelta #t))
      ((= type ctrl_Mouse)
       (let ((delta (INL_GetMouseDelta)))
         (set! dx (truncate (ref delta 0)))
         (set! dy (truncate (ref delta 1))))
       (set! buttons (INL_GetMouseButtons))
       (set! realdelta #t)))
    (if realdelta
        (begin
          (set! mx (cond ((< dx 0) motion_Left) ((> dx 0) motion_Right) (else motion_None)))
          (set! my (cond ((< dy 0) motion_Up) ((> dy 0) motion_Down) (else motion_None))))
        (begin
          (set! dx (* mx 127))
          (set! dy (* my 127))))
    (set! in-x dx)
    (set! in-y dy)
    (set! in-xaxis mx)
    (set! in-yaxis my)
    (set! in-button0 (not (= 0 (bitwise-and buttons 1))))
    (set! in-button1 (not (= 0 (bitwise-and buttons 2))))
    (set! in-button2 (not (= 0 (bitwise-and buttons 4))))
    (set! in-button3 (not (= 0 (bitwise-and buttons 8))))
    (set! in-dir (ref DirTable (+ (* (+ my 1) 3) (+ mx 1))))))

;; The DOS waits below spun until the keyboard interrupt wrote LastScan. Here a
;; wait yields to the frame driver, which polls the keyboard and presents a
;; frame before it resumes the game.
(define in-yield #f)

(define (IN_Yield)
  (if in-yield
      (begin (in-yield #f) #t)
      #f))

;; ID_IN.C:832-841
(define (IN_WaitForKey)
  (let wait ()
    (cond ((= LastScan sc_None)
           (IN_Yield)
           (wait))
          (else
           (let ((result LastScan))
             (set! LastScan sc_None)
             result)))))

;; ID_IN.C:851-859
(define (IN_WaitForASCII)
  (let wait ()
    (cond ((= LastASCII key_None)
           (IN_Yield)
           (wait))
          (else
           (let ((result LastASCII))
             (set! LastASCII key_None)
             result)))))

;; ID_IN.C:825-838, keyboard only
(define (IN_StartAck)
  (IN_ClearKeysDown)
  (let ((buttons (bitwise-ior (arithmetic-shift 0 4)
                              (if MousePresent (INL_GetMouseButtons) 0))))
    (let reset ((index 0) (remaining buttons))
      (when (< index 8)
        (setf! btnstate index (not (= 0 (bitwise-and remaining 1))))
        (reset (+ index 1) (arithmetic-shift remaining -1))))))

;; ID_IN.C:844-864, keyboard only
(define (IN_CheckAck)
  (if (not (= LastScan sc_None))
      #t
      (let ((buttons (bitwise-ior (arithmetic-shift 0 4)
                                  (if MousePresent (INL_GetMouseButtons) 0))))
        (let check ((index 0) (remaining buttons))
          (if (= index 8)
              #f
              (if (= 0 (bitwise-and remaining 1))
                  (begin
                    (setf! btnstate index #f)
                    (check (+ index 1) (arithmetic-shift remaining -1)))
                  (if (not (ref btnstate index))
                      #t
                      (check (+ index 1) (arithmetic-shift remaining -1)))))))))

;; ID_IN.C:869-873
(define (IN_Ack)
  (IN_StartAck)
  (let wait ()
    (unless (IN_CheckAck)
      (IN_Yield)
      (wait))))

;; ID_IN.C:935-949
(define (IN_UserInput delay)
  (let ((lasttime TimeCount))
    (IN_StartAck)
    (let wait ()
      (cond ((IN_CheckAck) #t)
            ((< (- TimeCount lasttime) delay)
             (IN_Yield)
             (wait))
            (else #f)))))
