;;; ID_SD.C — PC speaker and digitized sound

(define HITWALLSND 0)
(define SELECTWPNSND 1)
(define SELECTITEMSND 2)
(define HEARTBEATSND 3)
(define MOVEGUN2SND 4)
(define MOVEGUN1SND 5)
(define NOWAYSND 6)
(define NAZIHITPLAYERSND 7)
(define SCHABBSTHROWSND 8)
(define PLAYERDEATHSND 9)
(define DOGDEATHSND 10)
(define ATKGATLINGSND 11)
(define GETKEYSND 12)
(define NOITEMSND 13)
(define WALK1SND 14)
(define WALK2SND 15)
(define TAKEDAMAGESND 16)
(define GAMEOVERSND 17)
(define OPENDOORSND 18)
(define CLOSEDOORSND 19)
(define DONOTHINGSND 20)
(define HALTSND 21)
(define DEATHSCREAM2SND 22)
(define ATKKNIFESND 23)
(define ATKPISTOLSND 24)
(define DEATHSCREAM3SND 25)
(define ATKMACHINEGUNSND 26)
(define HITENEMYSND 27)
(define SHOOTDOORSND 28)
(define DEATHSCREAM1SND 29)
(define GETMACHINESND 30)
(define GETAMMOSND 31)
(define SHOOTSND 32)
(define HEALTH1SND 33)
(define HEALTH2SND 34)
(define BONUS1SND 35)
(define BONUS2SND 36)
(define BONUS3SND 37)
(define GETGATLINGSND 38)
(define ESCPRESSEDSND 39)
(define LEVELDONESND 40)
(define DOGBARKSND 41)
(define ENDBONUS1SND 42)
(define ENDBONUS2SND 43)
(define BONUS1UPSND 44)
(define BONUS4SND 45)
(define PUSHWALLSND 46)
(define NOBONUSSND 47)
(define PERCENT100SND 48)
(define BOSSACTIVESND 49)
(define MUTTISND 50)
(define SCHUTZADSND 51)
(define AHHHGSND 52)
(define DIESND 53)
(define EVASND 54)
(define GUTENTAGSND 55)
(define LEBENSND 56)
(define SCHEISTSND 57)
(define NAZIFIRESND 58)
(define BOSSFIRESND 59)
(define SSFIRESND 60)
(define SLURPIESND 61)
(define TOT_HUNDSND 62)
(define MEINGOTTSND 63)
(define SCHABBSHASND 64)
(define GETAMMOBOXSND 64)
(define HITLERHASND 65)
(define SPIONSND 66)
(define NEINSOVASSND 67)
(define DOGATTACKSND 68)
(define FLAMETHROWERSND 69)
(define MECHSTEPSND 70)
(define GOOBSSND 71)
(define YEAHSND 72)
(define DEATHSCREAM4SND 73)
(define DEATHSCREAM5SND 74)
(define DEATHSCREAM6SND 75)
(define DEATHSCREAM7SND 76)
(define DEATHSCREAM8SND 77)
(define DEATHSCREAM9SND 78)
(define DONNERSND 79)
(define EINESND 80)
(define ERLAUBENSND 81)
(define KEINSND 82)
(define MEINSND 83)
(define ROSESND 84)
(define MISSILEFIRESND 85)
(define MISSILEHITSND 86)
(define LASTSOUND 87)

(define sds_Off 0)
(define sds_PC 1)
(define sds_SoundSource 2)
(define sds_SoundBlaster 3)

(define sdm_Off 0)
(define sdm_PC 1)
(define sdm_AdLib 2)

;; AUDIOWL6.H:108-111
(define NUMSOUNDS 87)
(define STARTPCSOUNDS 0)
(define STARTADLIBSOUNDS 87)

(define DIGIHERTZ 7000)

(define SoundMode sdm_Off)
(define SoundNumber 0)
(define SoundPriority 0)
(define SoundTable STARTPCSOUNDS)
(define SoundUserHook #f)
(define SD_Started #f)

(define DigiMode sds_Off)
(define AdLibPresent #f)
(define SoundBlasterPresent #t)
(define SoundSourcePresent #t)
(define SBProPresent #t)
(define SBDMAChannel 0)
(define ssPort 2)
(define DigiList 0)
(define NumDigi 0)
(define DigiNumber 0)
(define DigiPriority 0)
(define DigiLeft 0)
(define DigiNextAddr #f)
(define DigiNextLen 0)
(define ssSample #f)
(define DigiMissed #f)
(define DigiPlaying #f)
(define DigiLastSegment #f)
(define DigiLastStart 1)
(define DigiLastEnd 0)
(define DigiMap (make-vector LASTSOUND -1))
(define LeftPosition 0)
(define RightPosition 0)
(define nextsoundpos #f)
(define SoundPositioned #f)

(define wolfdigimap
  (vector HALTSND 0
          DOGBARKSND 1
          CLOSEDOORSND 2
          OPENDOORSND 3
          ATKMACHINEGUNSND 4
          ATKPISTOLSND 5
          ATKGATLINGSND 6
          SCHUTZADSND 7
          GUTENTAGSND 8
          MUTTISND 9
          BOSSFIRESND 10
          SSFIRESND 11
          DEATHSCREAM1SND 12
          DEATHSCREAM2SND 13
          DEATHSCREAM3SND 13
          TAKEDAMAGESND 14
          PUSHWALLSND 15
          LEBENSND 20
          NAZIFIRESND 21
          SLURPIESND 22
          YEAHSND 32
          DOGDEATHSND 16
          AHHHGSND 17
          DIESND 18
          EVASND 19
          TOT_HUNDSND 23
          MEINGOTTSND 24
          SCHABBSHASND 25
          HITLERHASND 26
          SPIONSND 27
          NEINSOVASSND 28
          DOGATTACKSND 29
          LEVELDONESND 30
          MECHSTEPSND 31
          SCHEISTSND 33
          DEATHSCREAM4SND 34
          DEATHSCREAM5SND 35
          DONNERSND 36
          EINESND 37
          ERLAUBENSND 38
          DEATHSCREAM6SND 39
          DEATHSCREAM7SND 40
          DEATHSCREAM8SND 41
          DEATHSCREAM9SND 42
          KEINSND 43
          MEINSND 44
          ROSESND 45))

(define (InitDigiMap)
  (let loop ((index 0))
    (when (< index (vector-length wolfdigimap))
      (setf! DigiMap (ref wolfdigimap index) (ref wolfdigimap (+ index 1)))
      (loop (+ index 2)))))

(define (SDL_SetupDigi)
  (let ((table (PM_GetPage (- ChunksInFile 1))))
    (let count ((index 0) (page PMSoundStart))
      (if (or (= index (quotient PMPageSize 4)) (>= page (- ChunksInFile 1)))
          (begin
            (set! NumDigi index)
            (set! DigiList (make-vector (* index 2) 0))
            (let copy ((entry 0))
              (when (< entry index)
                (setf! DigiList (* entry 2) (readu16 table (* entry 4)))
                (setf! DigiList (+ (* entry 2) 1) (readu16 table (+ (* entry 4) 2)))
                (copy (+ entry 1)))))
          (count (+ index 1)
                 (+ page (quotient (+ (readu16 table (+ (* index 4) 2)) (- PMPageSize 1))
                                   PMPageSize)))))
    (let clear ((sound 0))
      (when (< sound LASTSOUND)
        (setf! DigiMap sound -1)
        (clear (+ sound 1))))))

;;; PC speaker
;;
;; The speaker plays a square wave. A sound holds one byte per 140 Hz service tick (TickBase*2,
;; ID_SD.C:237). The byte indexes pcSoundLookup, which holds i*60 (ID_SD.C:1995) and goes straight
;; to the 8253 channel 2 divisor, so the pitch is the timer base over that divisor. Zero is silence
;; (ID_SD.C:936-960).

(define TimerRate 0)
(define TimerDivisor 0)

(define (SDL_SetTimer0 speed)
  (if (= TimerDivisor (quotient 1192030 (* 70 100)))
      (set! TimerDivisor (quotient 1192030 (* 70 10)))
      (set! TimerDivisor speed)))

(define (SDL_SetIntsPerSec interrupts)
  (set! TimerRate interrupts)
  (SDL_SetTimer0 (quotient 1192030 interrupts)))

(define (SDL_SetTimerSpeed)
  (let ((rate (cond ((and (= DigiMode sds_PC) DigiPlaying) 7000)
                    ((or (= MusicMode smm_AdLib)
                         (and (= DigiMode sds_SoundSource) DigiPlaying))
                     700)
                    (else 140))))
    (unless (= rate TimerRate)
      (SDL_SetIntsPerSec rate)
      (set! TimerRate rate))))

(define PCBASETIMER 1193181)
(define PCSERVICERATE 140)
;; 44100 divides by the 140 Hz service rate exactly, so a tick is 315 samples with no drift, and
;; the square edges land within 23 microseconds of where the timer would have put them.
(define PCRATE 44100)
(define PCHIGH 200)
(define PCLOW 56)
(define PCSILENCE 128)

;; SoundCommon is a four-byte length then a two-byte priority (ID_SD.H:29-33).
(define SOUNDCOMMONSIZE 6)

(define pcSoundLookup (make-vector 255 0))
(define pcSamplePlaying #f)

(define (InitPCSoundLookup)
  (let loop ((index 0))
    (when (< index 255)
      (setf! pcSoundLookup index (* index 60))
      (loop (+ index 1)))))

(define (sound-samples data length)
  (if plus-enabled
      (let ((samples (make-bytevector length PCSILENCE)))
        (let copy ((index 0))
          (when (< index length)
            (setf! samples index (+ PCSILENCE (quotient (- (ref data index) PCSILENCE) 2)))
            (copy (+ index 1))))
        samples)
      (bytevector-copy data 0 length)))

(define (pc-render data length)
  (let* ((persample (truncate (/ PCRATE PCSERVICERATE)))
         (high (if plus-enabled (+ PCSILENCE (quotient (- PCHIGH PCSILENCE) 2)) PCHIGH))
         (low (if plus-enabled (+ PCSILENCE (quotient (- PCLOW PCSILENCE) 2)) PCLOW))
         (pcm (make-bytevector (* length persample) PCSILENCE)))
    (let ticks ((tick 0) (out 0) (phase 0))
      (if (= tick length)
          pcm
          (let ((sample (ref data (+ SOUNDCOMMONSIZE tick))))
            (if (= sample 0)
                (ticks (+ tick 1) (+ out persample) phase)
                (let ((step (/ (/ PCBASETIMER (ref pcSoundLookup sample)) PCRATE)))
                  (let emit ((offset 0) (turn phase))
                    (if (= offset persample)
                        (ticks (+ tick 1) (+ out persample) turn)
                        (let* ((moved (+ turn step))
                               (fraction (- moved (truncate moved))))
                          (setf! pcm (+ out offset) (if (< fraction 0.5) high low))
                          (emit (+ offset 1) fraction)))))))))))

(define (sound-chunk sound)
  (let ((chunk (+ SoundTable sound)))
    (if (eqv? (ref audiosegs chunk) 0)
        (CA_CacheAudioChunk chunk)
        (ref audiosegs chunk))))

(define (SDL_PCPlaySample data length)
  (dos:play-sound 'pc (pc-render data length) PCRATE)
  (set! pcSamplePlaying #t))

(define (SDL_PCPlaySound sound)
  (let* ((data (sound-chunk sound))
         (length (readu32 data 0)))
    (dos:play-sound 'pc (pc-render data length) PCRATE)
    (set! pcSamplePlaying #t)))

(define (SDL_PCStopSound)
  (set! pcSamplePlaying #f)
  (dos:stop-sound 'pc))

(define (SDL_PCStopSample)
  (SDL_PCStopSound))

(define (SDL_SoundFinished)
  (set! SoundNumber 0)
  (set! SoundPriority 0))

(define (SDL_PCService)
  (when (and pcSamplePlaying (not (dos:sound-playing? 'pc)))
    (SDL_PCStopSound)
    (SDL_SoundFinished)))

(define (SDL_ShutPC)
  (SDL_PCStopSound))

(define (SDL_ShutDevice)
  (cond ((= SoundMode sdm_PC) (SDL_ShutPC))
        ((= SoundMode sdm_AdLib) (SDL_ShutAL)))
  (set! SoundMode sdm_Off))

(define (SDL_CleanDevice)
  (when (or (= SoundMode sdm_AdLib) (= MusicMode smm_AdLib))
    (SDL_CleanAL)))

(define (SDL_StartDevice)
  (when (= SoundMode sdm_AdLib)
    (SDL_StartAL))
  (set! SoundNumber 0)
  (set! SoundPriority 0))

(define (SDL_LoadDigiSegment page)
  (let ((address (PM_GetPage (+ PMSoundStart page))))
    (PM_SetPageLock (+ PMSoundStart page) pml_Locked)
    address))

(define (SDL_PlayDigiSegment address length)
  (cond ((= DigiMode sds_PC) (SDL_PCPlaySample address length))
        ((= DigiMode sds_SoundSource) (SDL_SSPlaySample address length))
        ((= DigiMode sds_SoundBlaster) (SDL_SBPlaySample address length))))

(define (SDL_SBStopSample)
  (dos:stop-sound 'digi))

(define (SDL_SBPlaySeg data length)
  (dos:play-sound 'digi (sound-samples data length) DIGIHERTZ)
  length)

(define (SDL_SBService)
  (unless (dos:sound-playing? 'digi)
    (SDL_SBStopSample)
    (SDL_DigitizedDone)))

(define (SDL_SBPlaySample data length)
  (SDL_SBStopSample)
  (SDL_SBPlaySeg data length))

(define (SDL_PositionSBP leftpos rightpos)
  (when SBProPresent
    (dos:set-sound-attenuation leftpos rightpos)))

(define (SDL_CheckSB port)
  SoundBlasterPresent)

(define (SDL_DetectSoundBlaster port)
  (SDL_CheckSB port))

(define (SDL_SBSetDMA channel)
  (if (> channel 3)
      (Quit "SDL_SBSetDMA() - invalid SoundBlaster DMA channel")
      (set! SBDMAChannel channel)))

(define (SDL_StartSB)
  #f)

(define (SDL_ShutSB)
  (SDL_SBStopSample))

(define (SDL_SSStopSample)
  (set! ssSample #f)
  (dos:stop-sound 'digi))

(define (SDL_SSService)
  (unless (dos:sound-playing? 'digi)
    (SDL_SSStopSample)
    (SDL_DigitizedDone)))

(define (SDL_SSPlaySample data length)
  (set! ssSample data)
  (dos:play-sound 'digi (sound-samples data length) DIGIHERTZ))

(define (SDL_StartSS)
  #f)

(define (SDL_ShutSS)
  #f)

(define (SDL_CheckSS)
  SoundSourcePresent)

(define (SDL_DetectSoundSource)
  (let probe ((port 1))
    (set! ssPort port)
    (if (SDL_CheckSS)
        #t
        (if (= port 3)
            (begin (set! ssPort 4) #f)
            (probe (+ port 1))))))

(define smm_Off 0)
(define smm_AdLib 1)
(define alChar #x20)
(define alScale #x40)
(define alAttack #x60)
(define alSus #x80)
(define alFreqL #xa0)
(define alFreqH #xb0)
(define alEffects #xbd)
(define alFeedCon #xc0)
(define alWave #xe0)
(define MusicMode smm_Off)
(define sqActive #f)
(define sqPosition 2)
(define sqLength 0)
(define sqTime 0)
(define sqNextTime 0)
(define alSound #f)
(define alSoundPosition 0)
(define alSoundLength 0)
(define alBlock 0)
(define alServiceTime (time-monotonic))
(define alEffectTicks 0)
(define alMusicTicks 0)
(define alHookTicks 0)

(define (instrument-ref sound offset)
  (if sound (ref sound (+ SOUNDCOMMONSIZE offset)) 0))

(define (SDL_ALStopSound)
  (set! alSound #f)
  (set! alSoundLength 0)
  (alOut alFreqH 0))

(define (SDL_AlSetFXInst sound)
  (alOut alChar (instrument-ref sound 0))
  (alOut alScale (instrument-ref sound 2))
  (alOut alAttack (instrument-ref sound 4))
  (alOut alSus (instrument-ref sound 6))
  (alOut alWave (instrument-ref sound 8))
  (alOut (+ alChar 3) (instrument-ref sound 1))
  (alOut (+ alScale 3) (instrument-ref sound 3))
  (alOut (+ alAttack 3) (instrument-ref sound 5))
  (alOut (+ alSus 3) (instrument-ref sound 7))
  (alOut (+ alWave 3) (instrument-ref sound 9))
  (alOut alFeedCon 0))

(define (SDL_ALSoundService)
  (when alSound
    (let ((sample (ref alSound alSoundPosition)))
      (set! alSoundPosition (+ alSoundPosition 1))
      (if (= sample 0)
          (alOut alFreqH 0)
          (begin
            (alOut alFreqL sample)
            (alOut alFreqH alBlock))))
    (set! alSoundLength (- alSoundLength 1))
    (when (= alSoundLength 0)
      (SDL_ALStopSound)
      (SDL_SoundFinished))))

(define (reset-music-sequence)
  (set! sqPosition 2)
  (set! sqLength (readu16 current-music 0))
  (set! sqTime 0)
  (set! sqNextTime 0))

(define (SDL_ALService)
  (when sqActive
    (let events ()
      (when (and (> sqLength 0) (<= sqNextTime sqTime))
        (let ((word (readu16 current-music sqPosition))
              (delay (readu16 current-music (+ sqPosition 2))))
          (alOut (bitwise-and word #xff) (arithmetic-shift word -8))
          (set! sqPosition (+ sqPosition 4))
          (set! sqLength (- sqLength 4))
          (set! sqNextTime (+ sqTime delay))
          (events))))
    (set! sqTime (+ sqTime 1))
    (when (= sqLength 0)
      (reset-music-sequence))))

(define (SDL_ShutAL)
  (alOut alEffects 0)
  (alOut alFreqH 0)
  (SDL_AlSetFXInst #f)
  (set! alSound #f))

(define (SDL_CleanAL)
  (alOut alEffects 0)
  (let clear ((register 1))
    (when (<= register #xf5)
      (alOut register 0)
      (clear (+ register 1)))))

(define (SDL_StartAL)
  (alOut alEffects 0)
  (SDL_AlSetFXInst #f))

(define (SDL_DetectAdLib)
  (set! AdLibPresent (dos:adlib-reset))
  (when AdLibPresent
    (SDL_CleanAL)
    (alOut 1 #x20)
    (alOut 8 0)))

(define (run-service count service)
  (let loop ((left count))
    (when (> left 0)
      (service)
      (loop (- left 1)))))

(define (SDL_UserService)
  (when SoundUserHook
    (SoundUserHook)))

(define (SDL_t0Service)
  (let* ((now (time-monotonic))
         (elapsed (- now alServiceTime)))
    (set! alServiceTime now)
    (set! alEffectTicks (+ alEffectTicks (* elapsed 140)))
    (set! alMusicTicks (+ alMusicTicks (* elapsed 700)))
    (set! alHookTicks (+ alHookTicks (* elapsed 70)))
    (let ((effect-count (truncate alEffectTicks))
          (music-count (truncate alMusicTicks))
          (hook-count (truncate alHookTicks)))
      (set! alEffectTicks (- alEffectTicks effect-count))
      (set! alMusicTicks (- alMusicTicks music-count))
      (set! alHookTicks (- alHookTicks hook-count))
      (run-service effect-count SDL_ALSoundService)
      (run-service music-count SDL_ALService)
      (run-service hook-count SDL_UserService))))

(define (SDL_ALPlaySound sound)
  (SDL_ALStopSound)
  (let* ((data (sound-chunk sound))
         (length (readu32 data 0))
         (modifier-sustain (instrument-ref data 6))
         (carrier-sustain (instrument-ref data 7)))
    (when (= length 0)
      (Quit "SDL_ALPlaySound() - Zero length sound"))
    (when (> (+ SOUNDCOMMONSIZE 17 length) (bytevector-length data))
      (Quit "SDL_ALPlaySound() - Sound data is truncated"))
    (when (= (bitwise-ior modifier-sustain carrier-sustain) 0)
      (Quit "SDL_ALPlaySound() - Bad instrument"))
    (SDL_AlSetFXInst #f)
    (SDL_AlSetFXInst data)
    (set! alBlock (bitwise-ior
                    (arithmetic-shift (bitwise-and (ref data (+ SOUNDCOMMONSIZE 16)) 7) 2)
                    #x20))
    (set! alSound data)
    (set! alSoundPosition (+ SOUNDCOMMONSIZE 17))
    (set! alSoundLength length)))

(define (alOut register value)
  (dos:adlib-write register value))

(define (SD_MusicOff)
  (when (= MusicMode smm_AdLib)
    (alOut alEffects 0)
    (let mute ((track 0))
      (when (< track 10)
        (alOut (+ #xb1 track) 0)
        (mute (+ track 1)))))
  (set! sqActive #f))
(define (SD_MusicOn)
  (set! sqActive #t))
(define current-music #f)
(define (SD_StartMusic music)
  (SD_MusicOff)
  (when (= MusicMode smm_AdLib)
    (let ((length (readu16 music 0)))
      (when (or (= length 0) (not (= (remainder length 4) 0))
                (> (+ length 2) (bytevector-length music)))
        (Quit "SD_StartMusic() - Invalid music data"))
      (set! current-music music)
      (reset-music-sequence)
      (SD_MusicOn))))
(define (SD_FadeOutMusic)
  (when (= MusicMode smm_AdLib)
    (SD_MusicOff)
    (set! current-music #f)))

(define (SD_MusicPlaying)
  #f)

(define songs
  (vector 3 11 9 12 3 11 9 12 2 0
          8 18 17 4 8 18 4 17 2 1
          6 20 22 21 6 20 22 21 19 26
          3 11 9 12 3 11 9 12 2 0
          8 18 17 4 8 18 4 17 2 1
          6 20 22 21 6 20 22 21 19 15))

(define last-cp-music #f)

(define (StartCPMusic chunk)
  (when (and last-cp-music (not (= last-cp-music chunk)))
    (setf! audiosegs (+ STARTMUSIC last-cp-music) 0))
  (set! last-cp-music chunk)
  (SD_MusicOff)
  (let ((music (CA_CacheAudioChunk (+ STARTMUSIC chunk))))
    (SD_StartMusic music)))

(define (StartMusic)
  (StartCPMusic (ref songs (+ mapon (* episode 10)))))

(define (StopMusic)
  (SD_MusicOff)
  (let clear ((chunk STARTMUSIC))
    (when (< chunk NUMSNDCHUNKS)
      (setf! audiosegs chunk 0)
      (clear (+ chunk 1))))
  (set! last-cp-music #f)
  (set! current-music #f))

(define (FreeMusic)
  (StopMusic))

(define (SD_SetSoundMode mode)
  (SD_StopSound)
  (when (and (= mode sdm_AdLib) (not AdLibPresent))
    (set! mode sdm_PC))
  (let ((result (or (= mode sdm_Off) (= mode sdm_PC) (= mode sdm_AdLib))))
    (when (and result (not (= mode SoundMode)))
      (SDL_ShutDevice)
      (set! SoundMode mode)
      (set! SoundTable (if (= mode sdm_AdLib) STARTADLIBSOUNDS STARTPCSOUNDS))
      (SDL_StartDevice))
    (SDL_SetTimerSpeed)
    result))

(define (SD_SetMusicMode mode)
  (SD_FadeOutMusic)
  (let wait ()
    (when (SD_MusicPlaying)
      (wait)))
  (let ((result (or (= mode smm_Off) (and (= mode smm_AdLib) AdLibPresent))))
    (when result
      (set! MusicMode mode))
    (SDL_SetTimerSpeed)
    result))

(define (SD_Default gotit sound-mode music-mode)
  (let ((got-sound gotit)
        (got-music gotit))
    (when (and got-sound (= sound-mode sdm_AdLib) (not AdLibPresent))
      (set! got-sound #f))
    (unless got-sound
      (set! sound-mode (if AdLibPresent sdm_AdLib sdm_PC)))
    (unless (= sound-mode SoundMode)
      (SD_SetSoundMode sound-mode))
    (when (and got-music (= music-mode smm_AdLib) (not AdLibPresent))
      (set! got-music #f))
    (when (and (not got-music) AdLibPresent)
      (set! music-mode smm_AdLib))
    (unless (= music-mode MusicMode)
      (SD_SetMusicMode music-mode))))

(define (SD_Startup)
  (unless SD_Started
    (SD_SetSoundMode sdm_Off)
    (SD_SetMusicMode smm_Off)
    (SDL_DetectAdLib)
    (set! alServiceTime (time-monotonic))
    (set! alEffectTicks 0)
    (set! alMusicTicks 0)
    (set! alHookTicks 0)
    (set! SoundUserHook #f)
    (when SoundBlasterPresent
      (SDL_StartSB))
    (InitPCSoundLookup)
    (SDL_SetupDigi)
    (InitDigiMap)
    (set! SD_Started #t)))

(define (SD_SetUserHook hook)
  (set! SoundUserHook hook))

(define (SD_Shutdown)
  (when SD_Started
    (SD_MusicOff)
    (SD_StopSound)
    (SDL_ShutDevice)
    (SDL_CleanDevice)
    (when SoundBlasterPresent
      (SDL_ShutSB))
    (when SoundSourcePresent
      (SDL_ShutSS))
    (SDL_SetTimer0 0)
    (set! SD_Started #f)))

(define (SD_SetDigiDevice mode)
  (unless (= mode DigiMode)
    (SD_StopDigitized)
    (let ((device-not-present #f))
      (cond ((= mode sds_SoundBlaster)
             (unless SoundBlasterPresent
               (if SoundSourcePresent
                   (set! mode sds_SoundSource)
                   (set! device-not-present #t))))
            ((= mode sds_SoundSource)
             (unless SoundSourcePresent
               (set! device-not-present #t))))
      (unless device-not-present
        (when (= DigiMode sds_SoundSource)
          (SDL_ShutSS))
        (set! DigiMode mode)
        (when (= mode sds_SoundSource)
          (SDL_StartSS))
        (SDL_SetTimerSpeed)))))

(define (SD_Service)
  (SDL_t0Service))

(define (SD_Poll)
  (SDL_PCService)
  (when (and DigiPlaying
             (not (dos:sound-playing? (if (= DigiMode sds_PC) 'pc 'digi))))
    (SDL_DigitizedDone))
  (SDL_SetTimerSpeed))

(define (SD_SetPosition leftpos rightpos)
  (if (or (< leftpos 0) (> leftpos 15) (< rightpos 0) (> rightpos 15)
          (and (= leftpos 15) (= rightpos 15)))
      (Quit "SD_SetPosition: Illegal position")
      (when (= DigiMode sds_SoundBlaster)
        (SDL_PositionSBP leftpos rightpos))))

(define (SD_PositionSound leftvol rightvol)
  (set! LeftPosition leftvol)
  (set! RightPosition rightvol)
  (set! nextsoundpos #t))

(define (SD_SoundPlaying)
  (cond ((= SoundMode sdm_PC)
         (and (dos:sound-playing? 'pc) SoundNumber))
        ((= SoundMode sdm_AdLib)
         (and alSound SoundNumber))
        (else #f)))

(define (SD_WaitSoundDone)
  (let wait ()
    (when (SD_SoundPlaying)
      (SD_Service)
      (SD_Poll)
      (wait))))

(define (SD_StopDigitized)
  (set! DigiLeft 0)
  (set! DigiNextAddr #f)
  (set! DigiNextLen 0)
  (set! DigiMissed #f)
  (set! DigiPlaying #f)
  (set! DigiNumber 0)
  (set! DigiPriority 0)
  (set! SoundPositioned #f)
  (when (and (= DigiMode sds_PC) (= SoundMode sdm_PC))
    (set! SoundNumber 0)
    (set! SoundPriority 0))
  (cond ((= DigiMode sds_PC) (SDL_PCStopSample))
        ((= DigiMode sds_SoundSource) (SDL_SSStopSample))
        ((= DigiMode sds_SoundBlaster) (SDL_SBStopSample)))
  (let unlock ((page DigiLastStart))
    (when (< page DigiLastEnd)
      (PM_SetPageLock (+ page PMSoundStart) pml_Unlocked)
      (unlock (+ page 1))))
  (set! DigiLastStart 1)
  (set! DigiLastEnd 0))

;; ID_SD.C:2222-2243
(define (SD_StopSound)
  (when DigiPlaying
    (SD_StopDigitized))
  (cond ((= SoundMode sdm_PC) (SDL_PCStopSound))
        ((= SoundMode sdm_AdLib) (SDL_ALStopSound)))
  (set! SoundPositioned #f)
  (SDL_SoundFinished))

(define (SD_PlayDigitized which leftpos rightpos)
  (unless (= DigiMode sds_Off)
    (SD_StopDigitized)
    (when (>= which NumDigi)
      (Quit "SD_PlayDigitized: bad sound number"))
    (SD_SetPosition leftpos rightpos)
    (let ((first (ref DigiList (* which 2)))
          (total (ref DigiList (+ (* which 2) 1))))
      (set! DigiLastStart first)
      (set! DigiLastEnd (+ first (quotient (+ total (- PMPageSize 1)) PMPageSize)))
      (let ((samples (make-bytevector total 0)))
        (let load ((done 0) (page first))
          (if (< done total)
              (let* ((source (SDL_LoadDigiSegment page))
                     (part (min (- total done) (bytevector-length source))))
                (let copy ((index 0))
                  (when (< index part)
                    (setf! samples (+ done index) (ref source index))
                    (copy (+ index 1))))
                (load (+ done part) (+ page 1)))
              (begin
                (SDL_PlayDigiSegment samples total)
                (set! DigiPlaying #t)
                (set! DigiLastSegment #t))))))))

(define (SDL_DigitizedDone)
  (if DigiNextAddr
      (begin
        (SDL_PlayDigiSegment DigiNextAddr DigiNextLen)
        (set! DigiNextAddr #f)
        (set! DigiMissed #f))
      (if DigiLastSegment
          (begin
            (set! DigiPlaying #f)
            (set! DigiLastSegment #f)
            (if (and (= DigiMode sds_PC) (= SoundMode sdm_PC))
                (SDL_SoundFinished)
                (begin
                  (set! DigiNumber 0)
                  (set! DigiPriority 0)))
            (set! SoundPositioned #f))
          (set! DigiMissed #t))))

;; C reads the priority from the chunk of the current SoundMode (ID_SD.C:2140).
(define (SD_SoundPriority sound)
  (readu16 (sound-chunk sound) 4))

;; ID_SD.C:2122-2202. A sound with a digitized version goes to the Sound Blaster; every other one,
;; and that is every pickup, falls through to the speaker.
(define (SD_PlaySound sound)
  (let ((leftpos LeftPosition)
        (rightpos RightPosition)
        (ispos nextsoundpos))
    (set! LeftPosition 0)
    (set! RightPosition 0)
    (set! nextsoundpos #f)
    (if (= sound -1)
        #f
        (let* ((data (sound-chunk sound))
               (priority (readu16 data 4))
               (which (ref DigiMap sound)))
          (cond
            ((and (not (= DigiMode sds_Off)) (not (= which -1)))
             (if (and (= DigiMode sds_PC) (= SoundMode sdm_PC))
                 (if (< priority SoundPriority)
                     #f
                     (begin
                       (SDL_PCStopSound)
                       (SD_PlayDigitized which leftpos rightpos)
                       (set! SoundPositioned ispos)
                       (set! SoundNumber sound)
                       (set! SoundPriority priority)
                       #t))
                 (begin
                   (when (and (not (= DigiPriority 0)) (= DigiNumber 0))
                     (Quit "SD_PlaySound: Priority without a sound"))
                   (if (< priority DigiPriority)
                       #f
                       (begin
                         (SD_PlayDigitized which leftpos rightpos)
                         (set! SoundPositioned ispos)
                         (set! DigiNumber sound)
                         (set! DigiPriority priority)
                         #t)))))
            ((= SoundMode sdm_Off) #f)
            ((= (readu32 data 0) 0)
             (Quit "SD_PlaySound() - Zero length sound"))
            ((< priority SoundPriority) #f)
            (else
             (if (= SoundMode sdm_PC)
                 (SDL_PCPlaySound sound)
                 (SDL_ALPlaySound sound))
             (set! SoundNumber sound)
             (set! SoundPriority priority)
             #f))))))
