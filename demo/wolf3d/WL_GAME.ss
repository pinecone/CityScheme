;;; WL_GAME.C — level setup, screen setup, and the game loop

(define secrettotal 0)
(define TimeCount 0)
(define oldscore 0)
(define spearx 0)
(define speary 0)
(define spearangle 0)
(define spearflag #f)
(define spear-mode #f)

(define ElevatorBackTo (vector 1 1 7 3 5 3))

(define globalsoundx 0)
(define globalsoundy 0)

(define ATABLEMAX 15)

(define righttable
  (vector 8 8 8 8 8 8 8 7 7 7 7 7 7 6 0 0 0 0 0 1 3 5 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 7 7 7 7 7 6 4 0 0 0 0 0 2 4 6 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 7 7 7 7 6 6 4 1 0 0 0 1 2 4 6 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 7 7 7 7 6 5 4 2 1 0 1 2 3 5 7 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 7 7 7 6 5 4 3 2 2 3 3 5 6 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 7 7 7 6 6 5 4 4 4 4 5 6 7 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 7 7 7 6 6 5 5 5 6 6 7 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 7 7 7 6 6 7 7 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8))

(define lefttable
  (vector 8 8 8 8 8 8 8 8 5 3 1 0 0 0 0 0 6 7 7 7 7 7 7 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 6 4 2 0 0 0 0 0 4 6 7 7 7 7 7 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 6 4 2 1 0 0 0 1 4 6 6 7 7 7 7 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 7 5 3 2 1 0 1 2 4 5 6 7 7 7 7 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 6 5 3 3 2 2 3 4 5 6 7 7 7 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 7 6 5 4 4 4 4 5 6 6 7 7 7 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 7 6 6 5 5 5 6 6 7 7 7 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 7 7 6 6 7 7 7 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8
          8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8 8))

(define leftchannel 0)
(define rightchannel 0)

(define (SetSoundLoc gx gy)
  (let* ((dx (- gx viewx))
         (dy (- gy viewy))
         (x (floor (/ (- (FixedByFrac dx viewcos) (FixedByFrac dy viewsin)) TILEGLOBAL)))
         (y (floor (/ (+ (FixedByFrac dy viewcos) (FixedByFrac dx viewsin)) TILEGLOBAL)))
         (row (min (abs x) (- ATABLEMAX 1)))
         (column (+ (max (- ATABLEMAX) (min y (- ATABLEMAX 1))) ATABLEMAX))
         (cell (+ (* row ATABLEMAX 2) column)))
    (set! leftchannel (ref lefttable cell))
    (set! rightchannel (ref righttable cell))))

(define (UpdateSoundLoc)
  (when SoundPositioned
    (SetSoundLoc globalsoundx globalsoundy)
    (SD_SetPosition leftchannel rightchannel)))

(define (PlaySoundLocTile sound tx ty)
  (PlaySoundLocGlobal sound
                      (+ (* tx TILEGLOBAL) (/ TILEGLOBAL 2))
                      (+ (* ty TILEGLOBAL) (/ TILEGLOBAL 2))))

(define (PlaySoundLocGlobal sound gx gy)
  (SetSoundLoc gx gy)
  (SD_PositionSound leftchannel rightchannel)
  (when (SD_PlaySound sound)
    (set! globalsoundx gx)
    (set! globalsoundy gy)))

;; WL_GAME.C:316-575. Each enemy group has a base range plus one range per difficulty tier; the
;; tier step is 36 for the guard family and 18 for mutants.
(define (enemy-tier tile base step spawn which x y)
  (cond ((and (>= tile base) (<= tile (+ base 3)))
         (spawn which x y (- tile base)))
        ((and (>= tile (+ base step)) (<= tile (+ base step 3)))
         (when (>= difficulty gd_medium)
           (spawn which x y (- tile base step))))
        ((and (>= tile (+ base step step)) (<= tile (+ base step step 3)))
         (when (>= difficulty gd_hard)
           (spawn which x y (- tile base step step))))))

(define (spawn-enemy-tile tile x y)
  (enemy-tier tile 108 36 SpawnStand en_guard x y)
  (enemy-tier tile 112 36 SpawnPatrol en_guard x y)
  (enemy-tier tile 116 36 SpawnStand en_officer x y)
  (enemy-tier tile 120 36 SpawnPatrol en_officer x y)
  (enemy-tier tile 126 36 SpawnStand en_ss x y)
  (enemy-tier tile 130 36 SpawnPatrol en_ss x y)
  (enemy-tier tile 134 36 SpawnStand en_dog x y)
  (enemy-tier tile 138 36 SpawnPatrol en_dog x y)
  (enemy-tier tile 216 18 SpawnStand en_mutant x y)
  (enemy-tier tile 220 18 SpawnPatrol en_mutant x y))

(define (ScanInfoPlane)
  (let rows ((y 0))
    (when (< y MAPSIZE)
      (let cols ((x 0))
        (when (< x MAPSIZE)
          (let ((tile (maptile 1 (+ (* y MAPSIZE) x))))
            (cond ((= tile 0) 0)
                  ((and (>= tile 19) (<= tile 22)) (SpawnPlayer x y (- tile 19)))
                  ((and (>= tile 23) (<= tile 74)) (SpawnStatic x y (- tile 23)))
                  ((= tile PUSHABLETILE)
                   (unless loadedgame
                     (set! secrettotal (+ secrettotal 1))))
                  ((= tile 124) (SpawnDeadGuard x y))
                  ((= tile 214) (SpawnBoss x y))
                  ((= tile 197) (SpawnGretel x y))
                  ((= tile 215) (SpawnGift x y))
                  ((= tile 179) (SpawnFat x y))
                  ((= tile 196) (SpawnSchabbs x y))
                  ((= tile 160) (SpawnFakeHitler x y))
                  ((= tile 178) (SpawnHitler x y))
                  ((= tile 224) (SpawnGhosts en_blinky x y))
                  ((= tile 225) (SpawnGhosts en_clyde x y))
                  ((= tile 226) (SpawnGhosts en_pinky x y))
                  ((= tile 227) (SpawnGhosts en_inky x y))
                  (else (spawn-enemy-tile tile x y))))
          (cols (+ x 1))))
      (rows (+ y 1)))))

;; WL_GAME.C:660-690. Every ambush marker becomes floor, whether an actor stands on it or not.
(define (ClearAmbushMarkers)
  (let rows ((y 0))
    (when (< y MAPSIZE)
      (let cols ((x 0))
        (when (< x MAPSIZE)
          (when (= (maptile 0 (+ (* y MAPSIZE) x)) AMBUSHTILE)
            (tileset! x y 0)
            (areaset! x y (ambush-area x y)))
          (cols (+ x 1))))
      (rows (+ y 1)))))

(define (SetupTileMap)
  (let rows ((y 0))
    (when (< y MAPSIZE)
      (let cols ((x 0))
        (when (< x MAPSIZE)
          (let ((tile (maptile 0 (+ (* y MAPSIZE) x))))
            (tileset! x y (if (< tile AREATILE) tile 0))
            (areaset! x y
                      (if (and (>= tile AREATILE) (< tile (+ AREATILE NUMAREAS)))
                          (- tile AREATILE)
                          0)))
          (cols (+ x 1))))
      (rows (+ y 1)))))

(define (ClearMemory)
  (PM_SetMainMemPurge 3)
  (SD_StopDigitized)
  (MM_SortMem))

(define (SetupGameLevel)
  (unless loadedgame
    (set! TimeCount 0)
    (set! secretcount 0)
    (set! secrettotal 0)
    (set! killcount 0)
    (set! killtotal 0)
    (set! treasurecount 0)
    (set! treasuretotal 0))
  (set! pwallstate 0)
  (set! playstate ex_stillplaying)
  (US_InitRndT (if (or demoplayback demorecord) 0
                   (bitwise-and (truncate (* (time-monotonic) 70)) 255)))
  (CA_CacheMap (+ mapon (* episode 10)))
  (set! mapon (- mapon (* episode 10)))
  (SetupTileMap)
  (InitActorList)
  (InitDoorList)
  (InitStaticList)
  (SpawnDoors)
  (ScanInfoPlane)
  (plus-level-loaded)
  (ClearAmbushMarkers)
  (CA_LoadAllSounds))

(define (DrawPlayBorderSides)
  (let ((xl (- 160 (truncate (/ viewwidth 2))))
        (yl (truncate (/ (- (- screenheight STATUSLINES) viewheight) 2))))
    (VWB_Bar 0 0 (- xl 1) (- screenheight STATUSLINES) 127)
    (VWB_Bar (+ xl viewwidth 1) 0 (- xl 2) (- screenheight STATUSLINES) 127)
    (VWB_Vlin (- yl 1) (+ yl viewheight) (- xl 1) 0)
    (VWB_Vlin (- yl 1) (+ yl viewheight) (+ xl viewwidth) 125)))

(define (DrawAllPlayBorderSides)
  (DrawPlayBorderSides))

(define (DrawPlayBorder)
  (VWB_Bar 0 0 320 (- screenheight STATUSLINES) 127)
  (let ((xl (- 160 (truncate (/ viewwidth 2))))
        (yl (truncate (/ (- (- screenheight STATUSLINES) viewheight) 2))))
    (VWB_Bar xl yl viewwidth viewheight 0)
    (VWB_Hlin (- xl 1) (+ xl viewwidth) (- yl 1) 0)
    (VWB_Hlin (- xl 1) (+ xl viewwidth) (+ yl viewheight) 125)
    (VWB_Vlin (- yl 1) (+ yl viewheight) (- xl 1) 0)
    (VWB_Vlin (- yl 1) (+ yl viewheight) (+ xl viewwidth) 125)
    (VWB_Plot (- xl 1) (+ yl viewheight) 124)))

(define (DrawAllPlayBorder)
  (DrawPlayBorder))

(define (DrawPlayScreen)
  (VL_FadeOut 0 255 0 0 0 30)
  (CA_CacheGrChunk STATUSBARPIC)
  (DrawPlayBorder)
  (VWB_DrawPic 0 (- screenheight STATUSLINES) STATUSBARPIC)
  (DrawFace)
  (DrawHealth)
  (DrawLives)
  (DrawLevel)
  (DrawAmmo)
  (DrawKeys)
  (DrawWeapon)
  (DrawScore))

(define MAXDEMOSIZE 8192)

(define (StartDemoRecord level)
  (set! demo-buffer (MM_GetPtr MAXDEMOSIZE))
  (MM_SetLock demo-buffer #t)
  (setf! demo-buffer 0 level)
  (set! demo-pointer 4)
  (set! demo-end MAXDEMOSIZE)
  (set! demo-lasttimecount TimeCount)
  (set! demorecord #t))

(define (FinishDemoRecord)
  (set! demorecord #f)
  (setu16! demo-buffer 1 demo-pointer)
  (let ((data (bytevector-copy demo-buffer 0 demo-pointer))
        (number (US_LineInput 88 80 "" #t 1 0)))
    (when number
      (let ((port (open-output-file (string-append "/tmp/DEMO" number ".WL6"))))
        (write-bytes data port)
        (close-output-port port)))
    (MM_FreePtr demo-buffer)))

(define (StartDemoPlayback bytes)
  (set! demo-buffer bytes)
  (set! mapon (ref bytes 0))
  (set! demo-pointer 4)
  (set! demo-end (readu16 bytes 1))
  (set! demo-lasttimecount TimeCount)
  (set! demoplayback #t)
  (set! demorecord #f)
  (set! tics DEMOTICS))

(define (FinishDemoPlayback)
  (set! demoplayback #f))

(define (RecordDemo)
  (let ((text (US_LineInput 80 80 "" #t 2 0)))
    (when text
      (let ((level (- (string->number text) 1)))
        (when (and (>= level 0) (< level 60))
          (NewGame gd_hard (truncate (/ level 10)))
          (set! mapon (- level (* 10 episode)))
          (StartDemoRecord level)
          (DrawPlayScreen)
          (VL_FadeIn 0 255 gamepal 30)
          (SetupGameLevel)
          (set! demo-lasttimecount TimeCount)
          (StartMusic)
          (PM_CheckMainMem)
          (set! fizzlein #t)
          (let play ()
            (when (= playstate ex_stillplaying)
              (PlayLoop)
              (IN_Yield)
              (play)))
          (set! demoplayback #f)
          (StopMusic)
          (VL_FadeOut 0 255 0 0 0 30)
          (ClearMemory)
          (FinishDemoRecord))))))

(define (PlayDemo bytes)
  (NewGame gd_hard 0)
  (StartDemoPlayback bytes)
  (DrawPlayScreen)
  (VL_FadeIn 0 255 gamepal 30)
  (SetupGameLevel)
  (set! demo-lasttimecount TimeCount)
  (StartMusic)
  (PM_CheckMainMem)
  (set! fizzlein #t)
  (let play ()
    (when (= playstate ex_stillplaying)
      (PlayLoop)
      (IN_Yield)
      (play)))
  (FinishDemoPlayback)
  (StopMusic)
  (VL_FadeOut 0 255 0 0 0 30)
  (ClearMemory))

(define DEATHROTATE 2)

(define (death-angle)
  (let* ((dx (- (ref actor-x killerobj) player-x))
         (dy (- player-y (ref actor-y killerobj)))
         (fangle (atan2 dy dx))
         (wrapped (if (< fangle 0) (+ (* pi 2) fangle) fangle)))
    (truncate (* (/ wrapped (* pi 2)) ANGLES))))

(define (rotate-to-killer iangle)
  (let* ((clockwise (if (> player-angle iangle)
                        (+ (- ANGLES player-angle) iangle)
                        (- iangle player-angle)))
         (counter (if (> player-angle iangle)
                      (- player-angle iangle)
                      (+ player-angle (- ANGLES iangle))))
         (toward (< clockwise counter)))
    (let spin ((curangle (cond ((and toward (> player-angle iangle)) (- player-angle ANGLES))
                               ((and (not toward) (< player-angle iangle)) (+ player-angle ANGLES))
                               (else player-angle))))
      (let* ((step (if toward (* tics DEATHROTATE) (- (* tics DEATHROTATE))))
             (change (cond ((and toward (> (+ curangle step) iangle)) (- iangle curangle))
                           ((and (not toward) (< (+ curangle step) iangle)) (- iangle curangle))
                           (else step))))
        (set! player-angle (+ player-angle change))
        (when (>= player-angle ANGLES) (set! player-angle (- player-angle ANGLES)))
        (when (< player-angle 0) (set! player-angle (+ player-angle ANGLES)))
        (ThreeDRefresh)
        (VL_WaitVBL 1)
        (CalcTics)
        (when (not (= (+ curangle change) iangle))
          (spin (+ curangle change)))))))

;; WL_GAME.C:1114-1213
(define (Died)
  (set! weapon -1)
  (SD_PlaySound PLAYERDEATHSND)
  (when (>= killerobj 0)
    (rotate-to-killer (death-angle)))
  (FinishPaletteShifts)
  (IN_ClearKeysDown)
  (FizzleFade (fizzle-source viewleft viewtop viewwidth viewheight 4)
              viewleft viewtop viewwidth viewheight 70 #f)
  (IN_UserInput 100)
  (SD_WaitSoundDone)
  (unless tedlevel
    (set! lives (- lives 1)))
  (when (> lives -1)
    (set! health 100)
    (set! weapon wp_pistol)
    (set! bestweapon wp_pistol)
    (set! chosenweapon wp_pistol)
    (set! ammo STARTAMMO)
    (set! keys 0)
    (set! attackframe 0)
    (set! attackcount 0)
    (set! weaponframe 0)
    (DrawKeys)
    (DrawWeapon)
    (DrawAmmo)
    (DrawHealth)
    (DrawFace)
    (DrawLives)))

(define (next-map)
  (cond ((= mapon 9) (set! mapon (ref ElevatorBackTo episode)))
        ((= playstate ex_secretlevel) (set! mapon 9))
        (else (set! mapon (+ mapon 1)))))

(define (level-finished)
  (set! keys 0)
  (DrawKeys)
  (VL_FadeOut 0 255 0 0 0 30)
  (ClearMemory)
  (LevelCompleted)
  (set! oldscore score)
  (next-map))

;; WL_GAME.C:1394-1466. Returns the exit code that ends the game, and runs at most `limit` tics
;; per level so a headless script terminates.
(define (GameLoop limit)
  (ClearMemory)
  (set! fontcolor 0)
  (set! backcolor 15)
  (DrawPlayScreen)
  (let level ((died #f))
    (unless loadedgame
      (set! score oldscore))
    (DrawScore)
    (if loadedgame
        (set! loadedgame #f)
        (SetupGameLevel))
    (set! ingame #t)
    (StartMusic)
    (PM_CheckMainMem)
    (if died
        (set! died #f)
        (PreloadGraphics))
    (set! fizzlein #t)
    (DrawLevel)
    (let tic ((count 0))
      (if (and (= playstate ex_stillplaying) (< count (max 0 limit)))
          (begin (PlayLoop) (tic (+ count 1)))
          (begin
            (StopMusic)
            (set! ingame #f)
            (cond ((or (= playstate ex_completed) (= playstate ex_secretlevel))
                 (level-finished)
                 (level #f))
                ((= playstate ex_died)
                 (Died)
                 (if (> lives -1)
                     (level #t)
                     (begin (CheckHighScore score (+ mapon 1)) playstate)))
                ((= playstate ex_victorious)
                 (VL_FadeOut 0 255 0 0 0 30)
                 (ClearMemory)
                 (Victory)
                 (ClearMemory)
                 (CheckHighScore score (+ mapon 1))
                 playstate)
                (else playstate)))))))
