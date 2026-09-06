;;; ID_CA.C — asset loading

(define NUMMAPS 60)
(define MAPPLANES 2)

(define extension "WL6")

(define gheadname "VGAHEAD.")
(define gfilename "VGAGRAPH.")
(define gdictname "VGADICT.")
(define mheadname "MAPHEAD.")
(define mfilename "GAMEMAPS.")
(define aheadname "AUDIOHED.")
(define afilename "AUDIOT.")

(define datadirs (list "" "/dropbox/WOLF3D/"))
(define datadir-option-error #f)

(define (datadir-argument)
  (let scan ((index 0) (value #f))
    (if (>= index (vector-length argv))
        value
        (if (string=? (ref argv index) "--datadir")
            (cond ((>= (+ index 1) (vector-length argv))
                   (set! datadir-option-error "--datadir requires a path")
                   #f)
                  ((string=? (ref argv (+ index 1)) "")
                   (set! datadir-option-error "--datadir requires a nonempty path")
                   #f)
                  (value
                   (set! datadir-option-error "--datadir can be supplied only once")
                   #f)
                  (else (scan (+ index 2) (ref argv (+ index 1)))))
            (scan (+ index 1) value)))))

(define (directory-path path)
  (if (char=? (string-ref path (- (string-length path) 1)) #\/)
      path
      (string-append path "/")))

(define (input-file? filename)
  (let ((port (open-input-file/maybe filename)))
    (if port
        (begin
          (close-input-port port)
          #t)
        #f)))

(define (find-datadir dirs)
  (cond ((null? dirs) "")
        ((input-file? (string-append (car dirs) mheadname extension)) (car dirs))
        (else (find-datadir (cdr dirs)))))

(define datadir-option (datadir-argument))
(define datadir (if datadir-option
                    (directory-path datadir-option)
                    (find-datadir datadirs)))

(define tinf 0)
(define mapon 0)
(define ca_levelbit 0)
(define ca_levelnum 0)
(define mapsegs (make-vector MAPPLANES 0))
(define mapheaderseg (make-vector NUMMAPS 0))
(define grstarts 0)
(define audiostarts 0)
(define mapfiledata 0)
(define audiofiledata 0)
(define grfiledata 0)
(define chunkexplen 0)
(define chunkcomplen 0)
(define debughandle #f)

(define (CA_OpenDebug)
  (set! debughandle (open-output-file "DEBUG.TXT")))

(define (CA_CloseDebug)
  (close-output-port debughandle))

(define (datafile name)
  (string-append datadir name extension))

(define (CA_CannotOpen filename)
  (Quit (string-append "Can't open " filename "!\n")))

(define (CA_FarRead handle destination length)
  (let read-next ((offset 0))
    (if (= offset length)
        #t
        (let ((value (read-char handle)))
          (if (eof-object? value)
              #f
              (begin
                (setf! destination offset (char->integer value))
                (read-next (+ offset 1))))))))

(define (CA_ReadFile filename destination)
  (let ((handle (open-input-file/maybe filename)))
    (if (not handle)
        #f
        (let ((contents (read-bytes/all handle)))
          (close-input-port handle)
          (if (> (bytevector-length contents) (bytevector-length destination))
              #f
              (let copy ((offset 0))
                (if (= offset (bytevector-length contents))
                    #t
                    (begin
                      (setf! destination offset (ref contents offset))
                      (copy (+ offset 1))))))))))

(define (CA_FarWrite handle source length)
  (let write-next ((offset 0))
    (if (= offset length)
        #t
        (begin
          (write-char (integer->char (ref source offset)) handle)
          (write-next (+ offset 1))))))

(define (CA_WriteFile filename source length)
  (let* ((previous (CA_LoadFile filename))
         (handle (open-output-file filename))
         (success (CA_FarWrite handle source length)))
    (when (and success previous (> (bytevector-length previous) length))
      (let copy-tail ((offset length))
        (when (< offset (bytevector-length previous))
          (write-char (integer->char (ref previous offset)) handle)
          (copy-tail (+ offset 1)))))
    (close-output-port handle)
    success))

(define (CA_LoadFile filename)
  (let ((handle (open-input-file/maybe filename)))
    (if (not handle)
        #f
        (let ((contents (read-bytes/all handle)))
          (close-input-port handle)
          contents))))

(define (CAL_GetGrChunkLength chunk)
  (let ((position (GRFILEPOS chunk)))
    (set! chunkexplen (readu32 grfiledata position))
    (set! chunkcomplen (- (GRFILEPOS (+ chunk 1)) position 4))))

(define (readu16 bv offset)
  (+ (ref bv offset)
     (* 256 (ref bv (+ offset 1)))))

(define (readi16 bv offset)
  (let ((value (readu16 bv offset)))
    (if (>= value 32768) (- value 65536) value)))

(define (readu32 bv offset)
  (+ (readu16 bv offset)
     (* 65536 (readu16 bv (+ offset 2)))))

(define (readi32 bv offset)
  (let ((value (readu32 bv offset)))
    (if (>= value 2147483648)
        (- value 4294967296)
        value)))

(define (readname bv offset limit)
  (let loop ((index 0) (text ""))
    (if (= index limit)
        text
        (let ((byte (ref bv (+ offset index))))
          (if (= byte 0)
              text
              (loop (+ index 1) (string-append text (string (integer->char byte)))))))))

(define FILEPOSSIZE 3)

(define (GRFILEPOS chunk)
  (let* ((offset (* chunk FILEPOSSIZE))
         (value (+ (ref grstarts offset)
                   (* 256 (ref grstarts (+ offset 1)))
                   (* 65536 (ref grstarts (+ offset 2))))))
    (if (= value 16777215) -1 value)))

(define maptype (struct 'maptype '(planestart planelength width height name)))

(define MAPTYPESIZE 38)

(define (maptype-read bv offset)
  (maptype (vector (readi32 bv offset)
                   (readi32 bv (+ offset 4))
                   (readi32 bv (+ offset 8)))
           (vector (readu16 bv (+ offset 12))
                   (readu16 bv (+ offset 14))
                   (readu16 bv (+ offset 16)))
           (readu16 bv (+ offset 18))
           (readu16 bv (+ offset 20))
           (readname bv (+ offset 22) 16)))

(define (RLEWtag) (readu16 tinf 0))

(define (headeroffset index) (readi32 tinf (+ 2 (* index 4))))

(define (CAL_SetupMapFile)
  (let ((filename (datafile mheadname)))
    (set! tinf (CA_LoadFile filename))
    (unless tinf
      (CA_CannotOpen filename)))
  (let ((filename (datafile mfilename)))
    (set! mapfiledata (CA_LoadFile filename))
    (unless mapfiledata
      (CA_CannotOpen filename)))
  (let loop ((index 0))
    (unless (= index NUMMAPS)
      (let ((pos (headeroffset index)))
        (unless (< pos 0)
          (setf! mapheaderseg index (maptype-read mapfiledata pos))))
      (loop (+ index 1))))
  (let loop ((plane 0))
    (unless (= plane MAPPLANES)
      (setf! mapsegs plane (make-bytevector (* 64 64 2) 0))
      (loop (+ plane 1)))))

(define (CAL_SetupAudioFile)
  (let ((filename (datafile aheadname)))
    (set! audiostarts (CA_LoadFile filename))
    (unless audiostarts
      (CA_CannotOpen filename)))
  (let ((filename (datafile afilename)))
    (set! audiofiledata (CA_LoadFile filename))
    (unless audiofiledata
      (CA_CannotOpen filename))))

(define (CAL_SetupGrFile)
  (let ((filename (datafile gdictname)))
    (set! grhuffman (CA_LoadFile filename))
    (unless grhuffman
      (CA_CannotOpen filename)))
  (set! grhuffman (CAL_OptimizeNodes grhuffman))
  (let ((filename (datafile gheadname)))
    (set! grstarts (CA_LoadFile filename))
    (unless grstarts
      (CA_CannotOpen filename)))
  (let ((filename (datafile gfilename)))
    (set! grfiledata (CA_LoadFile filename))
    (unless grfiledata
      (CA_CannotOpen filename)))
  (let ((table (CA_CacheGrChunk STRUCTPIC)))
    (let loop ((index 0))
      (unless (= index NUMPICS)
        (setf! pictable index (pictabletype (readu16 table (* index 4))
                                            (readu16 table (+ (* index 4) 2))))
        (loop (+ index 1))))))

(define (CA_Startup)
  (when datadir-option-error
    (Quit datadir-option-error))
  (when (and datadir-option (not (input-file? (datafile mheadname))))
    (CA_CannotOpen (datafile mheadname)))
  (CAL_SetupMapFile)
  (CAL_SetupGrFile)
  (CAL_SetupAudioFile)
  (set! mapon -1)
  (set! ca_levelbit 1)
  (set! ca_levelnum 0))

(define (CA_Shutdown)
  #f)

(define (audiostart index) (readi32 audiostarts (* index 4)))

(define (setu16! bv offset value)
  (setf! bv offset (bitwise-and value 255))
  (setf! bv (+ offset 1) (bitwise-and (arithmetic-shift value -8) 255)))

(define grhuffman 0)

(define (CAL_OptimizeNodes table)
  (let ((nodes (make-vector 255 0)))
    (let loop ((node 0))
      (if (= node 255)
          nodes
          (begin
            (setf! nodes node
                   (vector (let ((value (readu16 table (* node 4))))
                             (if (>= value 256) (- 255 value) value))
                           (let ((value (readu16 table (+ (* node 4) 2))))
                             (if (>= value 256) (- 255 value) value))))
            (loop (+ node 1)))))))

(define (huffbit0 table node) (ref (ref table node) 0))
(define (huffbit1 table node) (ref (ref table node) 1))

(define (CAL_HuffExpand source sourceoff dest length table screenhack)
  (let ((plane-length (quotient length 4))
        (output-length (if screenhack (* (quotient length 4) 4) length)))
    (let loop ((inptr sourceoff) (bitmask 1) (byte (ref source sourceoff)) (node 254) (written 0))
      (unless (= written output-length)
        (let ((value (if (= 0 (bitwise-and byte bitmask))
                         (huffbit0 table node)
                         (huffbit1 table node))))
          (let ((nextmask (if (= bitmask 128) 1 (arithmetic-shift bitmask 1)))
                (nextptr (if (= bitmask 128) (+ inptr 1) inptr)))
            (let ((nextbyte (if (= bitmask 128) (ref source (+ inptr 1)) byte)))
              (if (>= value 0)
                  (begin
                    (if screenhack
                        (let ((plane (quotient written plane-length))
                              (offset (remainder written plane-length)))
                          (setf! dest
                                 (+ (* (quotient offset 80) 320) (* (remainder offset 80) 4) plane)
                                 value))
                        (setf! dest written value))
                    (loop nextptr nextmask nextbyte 254 (+ written 1)))
                  (loop nextptr nextmask nextbyte (- -1 value) written)))))))))

(define NEARTAG 167)
(define FARTAG 168)

(define (CAL_CarmackExpand source sourceoff dest length)
  (let loop ((inptr sourceoff) (outword 0) (words (quotient length 2)))
    (when (> words 0)
      (let ((ch (readu16 source inptr)))
        (let ((chhigh (arithmetic-shift ch -8))
              (count (bitwise-and ch 255)))
          (cond
            ((and (= chhigh NEARTAG) (= count 0))
             (setu16! dest (* outword 2) (bitwise-ior ch (ref source (+ inptr 2))))
             (loop (+ inptr 3) (+ outword 1) (- words 1)))
            ((= chhigh NEARTAG)
             (let ((copyword (- outword (ref source (+ inptr 2)))))
               (let copy ((index 0))
                 (when (< index count)
                   (setu16! dest (* (+ outword index) 2) (readu16 dest (* (+ copyword index) 2)))
                   (copy (+ index 1))))
               (loop (+ inptr 3) (+ outword count) (- words count))))
            ((and (= chhigh FARTAG) (= count 0))
             (setu16! dest (* outword 2) (bitwise-ior ch (ref source (+ inptr 2))))
             (loop (+ inptr 3) (+ outword 1) (- words 1)))
            ((= chhigh FARTAG)
             (let ((copyword (readu16 source (+ inptr 2))))
               (let copy ((index 0))
                 (when (< index count)
                   (setu16! dest (* (+ outword index) 2) (readu16 dest (* (+ copyword index) 2)))
                   (copy (+ index 1))))
               (loop (+ inptr 4) (+ outword count) (- words count))))
            (else
             (setu16! dest (* outword 2) ch)
             (loop (+ inptr 2) (+ outword 1) (- words 1)))))))))

(define (CA_RLEWCompress source length dest rlewtag)
  (let ((words (quotient (+ length 1) 2)))
    (let compress ((sourceword 0) (destword 0))
      (if (= sourceword words)
          (* destword 2)
          (let ((value (readu16 source (* sourceword 2))))
            (let count-run ((count 1))
              (if (and (< (+ sourceword count) words)
                       (= (readu16 source (* (+ sourceword count) 2)) value))
                  (count-run (+ count 1))
                  (if (or (> count 3) (= value rlewtag))
                      (begin
                        (setu16! dest (* destword 2) rlewtag)
                        (setu16! dest (* (+ destword 1) 2) count)
                        (setu16! dest (* (+ destword 2) 2) value)
                        (compress (+ sourceword count) (+ destword 3)))
                      (let copy ((index 0))
                        (if (= index count)
                            (compress (+ sourceword count) (+ destword count))
                            (begin
                              (setu16! dest (* (+ destword index) 2) value)
                              (copy (+ index 1)))))))))))))

(define (CA_RLEWexpand source sourceoff dest length rlewtag)
  (let ((end (quotient length 2)))
    (let loop ((inptr sourceoff) (outword 0))
      (when (< outword end)
        (let ((value (readu16 source inptr)))
          (if (= value rlewtag)
              (let ((count (readu16 source (+ inptr 2)))
                    (repeated (readu16 source (+ inptr 4))))
                (let fill ((index 0))
                  (when (< index count)
                    (setu16! dest (* (+ outword index) 2) repeated)
                    (fill (+ index 1))))
                (loop (+ inptr 6) (+ outword count)))
              (begin
                (setu16! dest (* outword 2) value)
                (loop (+ inptr 2) (+ outword 1)))))))))

(define (CA_CacheMap mapnum)
  (set! mapon mapnum)
  (let ((header (ref mapheaderseg mapnum))
        (size (* 64 64 2)))
    (let loop ((plane 0))
      (when (< plane MAPPLANES)
        (let ((pos (ref (ref header 'planestart) plane))
              (expanded (readu16 mapfiledata (ref (ref header 'planestart) plane))))
          (let ((buffer2 (make-bytevector expanded 0)))
            (CAL_CarmackExpand mapfiledata (+ pos 2) buffer2 expanded)
            (CA_RLEWexpand buffer2 2 (ref mapsegs plane) size (RLEWtag))))
        (loop (+ plane 1))))))

(define (maptile plane index) (readu16 (ref mapsegs plane) (* index 2)))

(define (CA_CacheGrChunk chunk)
  (setf! grneeded chunk (bitwise-ior (ref grneeded chunk) ca_levelbit))
  (let ((cached (ref grsegs chunk)))
    (if (number? cached)
        (let ((position (GRFILEPOS chunk)))
          (if (< position 0)
              #f
              (CAL_ExpandGrChunk chunk grfiledata position)))
        cached)))

(define (CA_CacheScreen chunk)
  (let* ((position (GRFILEPOS chunk))
         (expanded (readu32 grfiledata position)))
    (CAL_HuffExpand grfiledata (+ position 4) framebuffer expanded grhuffman #t)
    (VW_MarkUpdateBlock 0 0 319 199)))

(define NUMSNDCHUNKS 288)
(define STARTPCSOUNDS 0)
(define STARTADLIBSOUNDS 87)
(define STARTDIGISOUNDS 174)
(define STARTMUSIC 261)

(define audiosegs (make-vector NUMSNDCHUNKS 0))
(define oldsoundmode 0)

(define (CA_CacheAudioChunk chunk)
  (let ((cached (ref audiosegs chunk)))
    (if (number? cached)
        (let* ((pos (audiostart chunk))
               (compressed (- (audiostart (+ chunk 1)) pos))
               (dest (make-bytevector compressed 0)))
          (let loop ((index 0))
            (when (< index compressed)
              (setf! dest index (ref audiofiledata (+ pos index)))
              (loop (+ index 1))))
          (setf! audiosegs chunk dest)
          dest)
        cached)))

(define (CA_LoadAllSounds)
  (if (= SoundMode sdm_Off)
      #f
      (begin
        (let ((start (if (= SoundMode sdm_PC) STARTPCSOUNDS STARTADLIBSOUNDS)))
          (let loop ((index 0))
            (unless (= index NUMSOUNDS)
              (CA_CacheAudioChunk (+ start index))
              (loop (+ index 1)))))
        (set! oldsoundmode SoundMode))))

(define (CA_UpLevel)
  (if (= ca_levelnum 7)
      (Quit "CA_UpLevel: Up past level 7!")
      (begin
        (set! ca_levelbit (arithmetic-shift ca_levelbit 1))
        (set! ca_levelnum (+ ca_levelnum 1)))))

(define (CA_DownLevel)
  (if (= ca_levelnum 0)
      (Quit "CA_DownLevel: Down past level 0!")
      (begin
        (set! ca_levelbit (arithmetic-shift ca_levelbit -1))
        (set! ca_levelnum (- ca_levelnum 1))
        (CA_CacheMarks))))

(define (CA_ClearMarks)
  (let loop ((index 0))
    (unless (= index NUMCHUNKS)
      (setf! grneeded index (bitwise-and (ref grneeded index) (- 255 ca_levelbit)))
      (loop (+ index 1)))))

(define (CA_ClearAllMarks)
  (let loop ((index 0))
    (if (= index NUMCHUNKS)
        (begin
          (set! ca_levelbit 1)
          (set! ca_levelnum 0))
        (begin
          (setf! grneeded index 0)
          (loop (+ index 1))))))

(define (CA_SetGrPurge)
  (CA_ClearMarks))

(define (CA_SetAllPurge)
  (CA_SetGrPurge))

(define (CA_CacheMarks)
  (let loop ((chunk 0))
    (unless (= chunk NUMCHUNKS)
      (when (and (not (= (bitwise-and (ref grneeded chunk) ca_levelbit) 0))
                 (number? (ref grsegs chunk)))
        (let ((position (GRFILEPOS chunk)))
          (unless (< position 0)
            (CAL_ExpandGrChunk chunk grfiledata position))))
      (loop (+ chunk 1)))))

(define STRUCTPIC 0)
(define NUMPICS 132)
(define NUMCHUNKS 149)
(define grsegs (make-vector NUMCHUNKS 0))
(define grneeded (make-bytevector NUMCHUNKS 0))
(define STARTTILE8 135)
(define STARTEXTERNS 136)
(define NUMTILE8 72)
(define BLOCK 64)

(define pictable (make-vector NUMPICS 0))
(define pictabletype (struct 'pictabletype '(width height)))

(define (CAL_ExpandGrChunk chunk source sourceoff)
  (let ((tile (and (>= chunk STARTTILE8) (< chunk STARTEXTERNS))))
    (let ((expanded (if tile (* BLOCK NUMTILE8) (readu32 source sourceoff)))
          (dataoff (if tile sourceoff (+ sourceoff 4))))
      (let ((dest (make-bytevector expanded 0)))
        (CAL_HuffExpand source dataoff dest expanded grhuffman #f)
        (setf! grsegs chunk dest)
        dest))))

