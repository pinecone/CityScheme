(include "ID_CA.ss")
(include "ID_PM.ss")
(include "WL_DRAW.ss")
(include "ID_VH.ss")
(include "WL_SPRITE.ss")
(include "WL_AGENT.ss")
(include "WL_DEBUG.ss")
(include "WL_ACT1.ss")
(include "WL_ACT2.ss")
(include "ID_SD.ss")
(include "ID_MM.ss")
(include "ID_US_1.ss")
(include "WL_STATE.ss")
(include "ID_IN.ss")
(include "WL_GAME.ss")
(include "WL_INTER.ss")
(include "WL_TEXT.ss")
(include "WL_MENU.ss")
(include "WL_MAIN.ss")
(include "ID_VL.ss")
(include "demo-options.ss")
(include "plus.ss")
(include "demo.ss")

(CheckForEpisodes)
(Patch386)
(InitGame)

(define title-frames 0)
(define title-work 0)
(define title-update (time-monotonic))

(define (update-title frame-start)
  (let ((now (time-monotonic)))
    (set! title-frames (+ title-frames 1))
    (set! title-work (+ title-work (- now frame-start)))
    (when (>= (- now title-update) 1)
      (dos:set-window-title
       (string-append "Wolfenstein 3D - "
                      (number->string (/ (round (/ (* 10 title-frames) title-work)) 10))
                      " fps"))
      (set! title-frames 0)
      (set! title-work 0)
      (set! title-update now))))

(define (run-game)
  (StartCPMusic INTROSONG)
  (PG13)
  (let outer ()
    (set! ingame #f)
    (DemoLoop)
    (set! ingame #t)
    (DrawPlayScreen)
    (if loadedgame
        (begin
          (set! startgame #f)
          (set! loadedgame #f)
          (StartMusic)
          (DrawLevel)
          (set! playstate ex_stillplaying))
        (start-level))
    (let play ()
      (if (game-step)
          (begin
            (StartCPMusic INTROSONG)
            (outer))
          (begin
            (IN_Yield)
            (play))))))

(define game #f)

(define (frame)
  (unless quitting
    (let ((frame-start (time-monotonic)))
      (IN_PollKeyboard)
      (unless demo-session
        (update-clock)
        (SD_Service))
      (coro/next game #f)
      ;; The host presents the current visible linear RAM image.  The explicit
      ;; display page remains for reference copies, but direct reference writes
      ;; (including FizzleFade's per-VBL steps) must be visible immediately.
      (dos:display-framebuffer framebuffer)
      (update-title frame-start))))

(if demo-headless
    (run-demo)
    (begin
      ;; sokol allows one image update per frame, so only the driver displays a framebuffer.
      (set! vl-vbl (lambda (count) (IN_Yield)))
      (set! game
        (let/coro yield ()
          (set! in-yield yield)
          (if demo-session
              (begin
                (run-demo)
                (dos:request-quit))
              (run-game))))
      (dos:frame-loop "Wolfenstein 3D" screenwidth screenheight 'crt frame)))
