; TDrive Config, ported to ACME from util/vdconfig.s (ca65) in the teensyrom-tdrive repository. It assembled to the same bytes before "Colour" in its color prompt became "Color".
; The ca65 macros are expanded at each use: PS is "jsr pstr" and a !pet line ending in 0, MSG wraps that in msgbeg/fpad, KEYS prints
; each key as 18, C_OPT, key, 146, C_SRC, text, and CMDS is cmdbeg plus pstr with a !tx string.
; TDrive Config: edit the Alt-button disk lists (/tdrive-config.txt on each unit) and show / change the mounted images, entirely in machine code.
; Loads at $0801 with a BASIC "10 SYS 2061" stub. Talks to the drives only through the KERNAL: the "!CONFIG[,W]" and "!STATE" host text
; channels (docs/DESIGN.md 23), "$/folder/" listings for the browser and "CD:" on the command channel (logical file 15 for the SD unit, 14 for
; the USB unit, open for the whole run: closing a command channel closes every channel of its unit). The only mailbox access of its own
; is the return to the TR menu (VIO_MENU, on Q).
; The file is kept as text in TXT (each line ends in CR) with an index of its lines; every change rebuilds the text in NEWB (one line replaced,
; removed, swapped or added), copies it back, re-indexes and re-parses the entries, then writes the whole file. Lines that are not entries
; (comments, blank lines) stay where they are.
; Buffers and tables are BSS at $2000-$4FFF (not in the PRG). The screen is in the lower-case character set: !pet swaps the case of the
; letters in its strings for that set, strings for the drive (!tx: CMDS, types) are as they are.
; Colors follow the TR menu: its seven color references (background, border, banner, time, option, sources, name) come as the third
; line of "!STATE" (hex digits; the menu's defaults when a firmware sends no such line). Bytes C_BAN..C_NAME in a screen string select
; one (fch sets the text color from THEME). As in the menu: a reverse banner, keys in reverse in the option color, their descriptions and
; labels in the sources color, paths and names in the name color, messages in the time color.
!cpu 6502

SETLFS  = $FFBA
SETNAM  = $FFBD
OPEN    = $FFC0
CLOSE   = $FFC3
CHKIN   = $FFC6
CHKOUT  = $FFC9
CLRCHN  = $FFCC
CHRIN   = $FFCF
CHROUT  = $FFD2
GETIN   = $FFE4
PLOT    = $FFF0
LISTEN  = $FFB1
SECOND  = $FF93
UNLSN   = $FFAE
STATUS  = $90
QTSW    = $D4                   ; quote mode of the screen editor: a '"' in a path must not turn the control codes after it into characters
INSRT   = $D8                   ; insert count, likewise
BORDER  = $D020
BGCOL   = $D021
TXTCOL  = $0286
DEV0REG = $DE16                 ; tdrive: current device number of the SD unit, $DE17 the USB unit (0 = none)
MBXCMD  = $DE10                 ; tdrive mailbox (d64/tdrive_io.h): command register ...
MBXSTAT = $DE13                 ; ... and its answer token
MBXARG  = $DE11                 ; argument (the device for the fast open)
MBXDATA = $DE12                 ; result byte (the fast open's error, a block's byte count)
MBXST   = $DE14                 ; IEC status bits of the answer ($40 EOI: the last block)
MBXLEN  = $DE15                 ; name length for the fast open
WIN     = $DF80                 ; the 128-byte IO2 window: the name out, the blocks in
WINSZ   = 128
C_FLOPEN = 9                    ; FLOAD_OPEN / FLOAD_BLK: the fast LOAD's block transfer (d64/tdrive_io.h), used for the browser's listings
C_FLBLK = 10
STAT_NOTMINE = $A5
STAT_SCAN = $3C
VIO_MENU = 16                   ; command: back to the TR menu (the Teensy resets the C64 into it)
STAT_OK = $5A                   ; answer token: done

P1      = $FB                   ; inline strings
P2      = $FD                   ; the string being printed, parsed or compared
P3      = $22                   ; write pointer; $22-$25 are BASIC's scratch pointers, unused while this runs
P4      = $24                   ; read pointer

MAXL    = 119                   ; lines kept from the file
MAXE    = 32                    ; list entries (what the firmware reads)
MAXB    = 100                   ; browser entries
SAVEMAX = 2000                  ; bytes of the file the firmware reads
VR      = 16                    ; list rows on the screen (rows 4-19)
BPMAX   = 200                   ; browser path
CURSOR  = $A4                   ; the line editor's cursor (an underscore)
LARROW  = 95                    ; left arrow: cancel; "CD:<-" unmounts
OUTSCR  = 0                     ; OUTM: where fch sends characters
OUTLINE = 1
OUTCMD  = 2
C_BAN   = 3                     ; screen string bytes 1-7: color reference 0-6 of the TR menu (THEME); only these five are used for text
C_TIME  = 4
C_OPT   = 5
C_SRC   = 6
C_NAME  = 7


* = $0801
        !wo @next
        !wo 10
        !tx $9E, "2061", 0
@next   !wo 0

start
!if start <> 2061 { !error "the SYS address in the BASIC stub must match" }
        jsr scr
        jsr pstr                ; the lower-case character set, and Shift+C= may not switch it back
        !pet 14, 8, 0
        jsr pstr
        !pet "Reading config...", 13, 0
        ldx #1                  ; the TR menu probed the bus before the launch, so DEV0/DEV1 are already settled
@dv     lda DEV0REG,x
        cmp #8
        bcc @no
        cmp #31
        bcc @ok
@no     lda #0
@ok     sta DEVS,x
        dex
        bpl @dv
        lda DEVS
        ora DEVS+1
        bne @act
        lda #255
        sta FLDN
        jsr pstr
        !pet "TDrive is not active.", 13, "Start this program from the TeensyROM menu with the TDrive KERNAL.", 13, 0
        rts
@act    lda BORDER
        and #15
        sta OC
        lda BGCOL
        and #15
        sta OB
        lda TXTCOL
        and #15
        sta OT
        lda DEVS
        beq @o1
        tax
        lda #15
        ldy #15
        jsr openc
@o1     lda DEVS+1
        beq @o2
        tax
        lda #14
        ldy #15
        jsr openc
@o2     lda #0
        sta UN
        lda DEVS
        bne @u
        inc UN                  ; no SD unit: start on the USB list
@u      jsr setunit
        jsr readstate
        lda THEME+1             ; the menu's border and background
        sta BORDER
        lda THEME
        sta BGCOL
        jsr readcfg
        lda #0
        sta CU
        sta TP
        jsr drawall
        jsr msgbeg
        jsr pstr
        !pet "Ready. Alt: 1 tap = SD, 2 taps = USB", 0
        jsr fpad
main    jsr getkey
        cmp #'Q'
        beq quit
        ldx #NKEYS-1
@f      cmp keytab,x
        beq @go
        dex
        bpl @f
        bmi main
@go     jsr @call
        jmp main
@call   lda keyhi,x
        pha
        lda keylo,x
        pha
        rts

keytab  !by 145, 17, 13, 'B', 'U', 'A', 'E', 'D', '+', '-', 'T', 'R'
NKEYS = * - keytab
keylo   !by <(k_up-1), <(k_down-1), <(k_ret-1), <(k_brow-1), <(k_unm-1), <(k_add-1), <(k_edit-1), <(k_del-1), <(k_plus-1), <(k_minus-1), <(k_tog-1), <(k_ref-1)
keyhi   !by >(k_up-1), >(k_down-1), >(k_ret-1), >(k_brow-1), >(k_unm-1), >(k_add-1), >(k_edit-1), >(k_del-1), >(k_plus-1), >(k_minus-1), >(k_tog-1), >(k_ref-1)

; Q: back to the TR menu. The Teensy answers the mailbox command, writes back what is unsaved and resets the C64 into its menu; if
; nothing answers (or no reset comes) this returns to BASIC instead.
quit    lda #15
        jsr CLOSE
        lda #14
        jsr CLOSE
        jsr CLRCHN
        sei                     ; nothing from the KERNAL while the Teensy writes to the card
        lda #VIO_MENU
        sta MBXCMD
        ldx #0
        ldy #0
@w      lda MBXSTAT             ; BUSY until answered; read twice, as the KERNAL does (a late answer gives a mixed byte)
        cmp #STAT_OK
        bne @n
        cmp MBXSTAT
        beq @ok
@n      dex
        bne @w
        dey
        bne @w
        beq @bas                ; not answered in about a second
@ok     lda #4                  ; answered: the reset comes within a few ms; give it about 4 s
        sta T0
@r      dex
        bne @r
        dey
        bne @r
        dec T0
        bne @r
@bas    cli
        lda OC
        sta BORDER
        lda OB
        sta BGCOL
        lda OT
        sta TXTCOL
        lda #255
        sta FLDN
        jsr pstr
        !tx 147, 142, 9, 154, "BYE", 0 ; upper case again for BASIC
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; open logical file A on device X, secondary Y, without a name (the command channel)
openc   jsr SETLFS
        lda #0
        jsr SETNAM
        jmp OPEN

; open logical file A on device X, secondary Y, with the name in CMD (CMDLEN bytes)
openn   jsr SETLFS
        lda CMDLEN
        ldx #<CMD
        ldy #>CMD
        jsr SETNAM
        jmp OPEN

; wait for a key
getkey  jsr GETIN
        beq getkey
        rts

; DV / EL: device and command channel of the unit whose list is shown
setunit ldx UN
        lda DEVS,x
        sta DV
        lda #15
        cpx #0
        beq @s
        lda #14
@s      sta EL
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Output. Everything goes through fch, which sends to the screen, to LINEB or to CMD (OUTM). On the screen, printable characters
; count down FLDN and are dropped at 0 (a field of fixed width, padded by fpad); control codes always go out.

; one character; keeps A, X, Y
fch     sta FCH
        stx FX
        sty FY
        ldx OUTM
        bne @buf
        cmp #C_NAME+1
        bcs @ch
        tax
        beq @ch
        lda THEME-1,x           ; 1-7: a color reference
        sta TXTCOL
        jmp @r
@ch     and #$7F
        cmp #$20
        bcc @out
        lda FLDN
        beq @r
        dec FLDN
@out    lda FCH
        jsr CHROUT
        lda #0
        sta QTSW
        sta INSRT
        beq @r
@buf    dex
        bne @cmd
        ldx LBLEN
        cpx #255
        bcs @r
        lda FCH
        sta LINEB,x
        inc LBLEN
        bne @r
@cmd    ldx CMDLEN
        cpx #255
        bcs @r
        lda FCH
        sta CMD,x
        inc CMDLEN
@r      ldx FX
        ldy FY
        lda FCH
        rts

scr     lda #OUTSCR
        sta OUTM
        rts

cmdbeg  lda #OUTCMD
        sta OUTM
        lda #0
        sta CMDLEN
        rts

; fch with a letter $41-$5A shown in upper case (lower-case character set): the types from the listing
fchu    cmp #'A'
        bcc @f
        cmp #'Z'+1
        bcs @f
        ora #$80
@f      jmp fch

; spaces up to the end of the field
fpad    lda FLDN
        beq @d
        lda #' '
        jsr fch
        jmp fpad
@d      rts

; print the string that follows the JSR (zero terminated)
pstr    pla
        sta P1
        pla
        sta P1+1
        ldy #1
@l      lda (P1),y
        beq @d
        jsr fch
        iny
        bne @l
@d      tya
        clc
        adc P1
        tax
        lda P1+1
        adc #0
        pha
        txa
        pha
        rts

; A in decimal, right justified in X columns (0 = no padding)
fnum    stx NW
        ldx #0
@l      ldy #0
@s      cmp #10
        bcc @r
        sbc #10
        iny
        bne @s
@r      ora #'0'
        sta DIGS,x
        inx
        tya
        bne @l
        stx ND
        lda NW
        sec
        sbc ND
        bcc @o
        beq @o
        tay
@p      lda #' '
        jsr fch
        dey
        bne @p
@o      ldx ND
@q      lda DIGS-1,x
        jsr fch
        dex
        bne @q
        rts

; the SLEN characters at (P2), from index 0 / from index Y
fmem    ldy #0
fmemy   cpy SLEN
        bcs @d
        lda (P2),y
        jsr fch
        iny
        bne fmemy
@d      rts

; (P2)/SLEN in a field of A columns
ffield  sta FLDN
        jsr fmem
        jmp fpad

; (P2)/SLEN in a field of A columns; when it is longer, ".." and its last A-2 characters
fpath   sta FLDN
        cmp SLEN
        bcs @all
        lda #'.'
        jsr fch
        jsr fch
        lda SLEN
        sec
        sbc FLDN
        tay
        jsr fmemy
        jmp fpad
@all    jsr fmem
        jmp fpad

; cursor to row X, column 0
atrow   ldy #0
        clc
        jmp PLOT

; cursor to row X, a field of 39 columns
rowbeg  jsr atrow
        lda #39
        sta FLDN
        rts

msgbeg  jsr scr
        ldx #20
        jsr rowbeg
        lda #C_TIME
        jmp fch

; "ERROR nn,message" on row 20
perr    jsr msgbeg
        jsr pstr
        !pet "Error ", 0
        lda ENUM
        ldx #0
        jsr fnum
        jsr pstr
        !pet ",", 0
        jsr pemsg
        jmp fpad

; ---------------------------------------------------------------------------------------------------------------------------------
; ---------------------------------------------------------------------------------------------------------------------------------
; Listings in blocks. A folder listing read with CHRIN costs one mailbox round trip per byte, which a folder of many files makes slow.
; The KERNAL's fast LOAD reads files 128 bytes per round trip through the IO2 window (FLOAD_OPEN, then FLOAD_BLK until EOI); the
; browser asks for its listings the same way (fopen / fget) and falls back to OPEN + CHRIN when that is not answered.

; one mailbox command A: C=0 answered (A = DATA, MST = ST), C=1 not (not mine, a bus-scan request, or no answer in about a second)
mcall   sta MBXCMD
        lda #0
        sta MW0
        sta MW1
@w      lda MBXSTAT
        cmp #STAT_OK
        beq @ok
        cmp #STAT_NOTMINE
        beq @no
        cmp #STAT_SCAN
        beq @no
        inc MW0
        bne @w
        inc MW1
        bne @w
@no     sec
        rts
@ok     lda MBXST               ; each answer byte read twice in a row until two agree, as the KERNAL does
        cmp MBXST
        bne @ok
        sta MST
@d      lda MBXDATA
        cmp MBXDATA
        bne @d
        clc
        rts

; open the listing named in CMD (CMDLEN bytes) on DV for fget: C=0 open; C=1 not, ENUM = the drive's error if it refused (else 0:
; a name of 128 bytes or more, or no answer: use OPEN + CHRIN)
fopen   lda #0
        sta FAST
        sta ENUM
        ldy CMDLEN
        bmi @no                 ; >= WINSZ: does not fit the window
        ldy #0
@nm     lda CMD,y               ; the name -> window, each byte read back
        sta WIN,y
        cmp WIN,y
        bne @nm
        iny
        cpy CMDLEN
        bne @nm
        sty MBXLEN
        lda DV
        sta MBXARG
        lda #C_FLOPEN
        jsr mcall
        bcs @no
        cmp #0
        bne @err
        lda #2                  ; the window holds the two load address bytes: fget hands them out first, as CHRIN would
        sta FN
        lda #0
        sta FI
        sta FEND
        lda #$80
        sta FAST
        clc
        rts
@err    jsr rdel                ; why (62 for a folder that is not there, ...)
@no     sec
        rts

; the next byte of the listing: from the block reader after fopen, else CHRIN
rdb     bit FAST
        bmi fget
        jmp CHRIN

; the next byte of a listing opened by fopen, with STATUS as CHRIN from a serial file leaves it: $40 with the last byte, $42 when
; nothing is left (a byte 0 then) or a block was not answered. X, Y kept.
fget    stx FGX
        ldx FI
        cpx FN
        bcc @have
        lda FEND
        bne @past               ; the last block was used up
        lda #C_FLBLK
        jsr mcall
        bcs @past
        sta FN                  ; 0..128 bytes in the window
        lda MST
        and #$40
        sta FEND                ; EOI: this is the last block
        ldx #0
        stx FI
        cpx FN
        bcc @have
@past   lda #$40
        sta FEND
        lda STATUS
        ora #$42
        sta STATUS
        lda #0
        ldx FGX
        rts
@have   lda WIN,x
        cmp WIN,x
        bne @have
        inx
        stx FI
        cpx FN
        bcc @out
        pha
        lda FEND
        beq @more
        lda STATUS              ; the last byte of the last block
        ora #$40
        sta STATUS
@more   pla
@out    ldx FGX
        rts

; Read the error channel of logical file A: ERRBUF = "NN,MESSAGE,TT,SS", ENUM = NN (255 = not understood).
rdel    lda EL
rderr   tax
        jsr CHKIN
        lda #0
        sta STATUS
        sta EI
@l      jsr CHRIN
        ldy EI
        cmp #13
        beq @e
        sta ERRBUF,y
        inc EI
        lda STATUS
        bne @e
        lda EI
        cmp #46
        bcc @l
@e      ldy EI
        lda #0
        sta ERRBUF,y
        sty ERRLEN
        jsr CLRCHN
        lda ERRBUF
        sec
        sbc #'0'
        cmp #10
        bcs @bad
        sta TMP
        asl
        asl
        adc TMP
        asl
        sta TMP
        lda ERRBUF+1
        sec
        sbc #'0'
        cmp #10
        bcs @bad
        clc
        adc TMP
        sta ENUM
        rts
@bad    lda #255
        sta ENUM
        rts

; the message part of the error channel text (after the first comma, without the leading spaces, up to the next comma)
pemsg   ldy #3
        cpy ERRLEN
        bcs @d
@s      lda ERRBUF,y
        cmp #' '
        bne @l
        iny
        bne @s
@l      lda ERRBUF,y
        beq @d
        cmp #','
        beq @d
        jsr fch
        iny
        bne @l
@d      rts

; send CMD and a CR to the command channel of the shown unit, then read its error channel
sendcmd jsr scr
        ldx EL
        jsr CHKOUT
        lda #0
        sta IX
@l      ldy IX
        cpy CMDLEN
        bcs @e
        lda CMD,y
        jsr CHROUT
        inc IX
        bne @l
@e      lda #13
        jsr CHROUT
        jsr CLRCHN
        jmp rdel

; ---------------------------------------------------------------------------------------------------------------------------------
; MPL / MPB: the image mounted on each unit, from the first line of "!STATE" (empty when none or no unit); THEME from its third line.
readstate
        ldx #1
@u      stx U
        lda #0
        sta MPL,x
        cpx #0
        beq @o
        lda #128
@o      sta UOFS
        lda DEVS,x
        bne @go
        jmp @nx
@go
        jsr cmdbeg
        jsr pstr
        !tx "!STATE", 0
        jsr scr
        ldx U
        lda DEVS,x
        tax
        lda #2
        ldy #2
        jsr openn
        lda #15
        ldx U
        beq @e
        lda #14
@e      jsr rderr
        lda ENUM
        bne @cl
        ldx #2
        jsr CHKIN
        lda #0
        sta STATUS
        sta LLEN
@rd     jsr CHRIN
        sta CH
        lda STATUS
        sta RS
        and #$BF
        bne @end
        lda CH
        cmp #13
        beq @end
        ldy LLEN
        cpy #127
        bcs @sk
        tya
        ora UOFS
        tax
        lda CH
        sta MPB,x
        inc LLEN
@sk     lda RS
        and #$40
        beq @rd
@end    lda RS                  ; 0: the line ended in CR with more to come
        bne @ce
        jsr rdtheme
@ce     jsr CLRCHN
        ldx U
        lda LLEN
        sta MPL,x
@cl     lda #2
        jsr CLOSE
@nx     ldx U
        dex
        bmi @x
        jmp @u
@x      rts

; the rest of "!STATE" (input from it): skip the folder line, then THEME = the NumColorRefs hex digits of the third line (all or nothing)
rdtheme jsr CHRIN
        sta CH
        lda STATUS
        bne @x
        lda CH
        cmp #13
        bne rdtheme
        lda #0
        sta T1
@h      jsr CHRIN
        sta CH
        lda STATUS
        and #$BF
        bne @x
        lda CH
        cmp #'0'
        bcc @x
        cmp #'9'+1
        bcc @dg
        cmp #'A'
        bcc @x
        cmp #'F'+1
        bcs @x
        sbc #'A'-10-1           ; carry clear: A-'A'+10
        jmp @st
@dg     and #15
@st     ldx T1
        sta THN,x
        inx
        stx T1
        cpx #7
        bcs @ok
        lda STATUS
        beq @h
@x      rts
@ok     dex
@cp     lda THN,x
        sta THEME,x
        dex
        bpl @cp
        rts

; P2 / SLEN = the image mounted on unit X
mpptr   lda MPL,x
        sta SLEN
        lda #<MPB
        cpx #0
        beq @z
        lda #<(MPB+128)
@z      sta P2
        lda #>MPB
        sta P2+1
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Read the shown unit's "!CONFIG" into TXT: lines end in CR, 0 bytes are dropped, at most MAXL lines of 255 characters
; (a missing file is an empty list). Then index and parse.
readcfg lda #<TXT
        sta P3
        lda #>TXT
        sta P3+1
        lda #(>TXT)+15
        sta LIMHI
        lda #0
        sta NLC
        sta LLEN
        jsr cmdbeg
        jsr pstr
        !tx "!CONFIG", 0
        jsr scr
        lda #2
        ldx DV
        ldy #2
        jsr openn
        jsr rdel
        lda ENUM
        bne @cl
        ldx #2
        jsr CHKIN
        lda #0
        sta STATUS
@rd     jsr CHRIN
        sta CH
        lda STATUS
        sta RS
        and #$BF
        bne @eof
        lda CH
        beq @nx
        cmp #13
        bne @c
        jsr endl
        jmp @nx
@c      lda LLEN
        cmp #255
        bcs @nx
        lda NLC
        cmp #MAXL
        bcs @nx
        lda CH
        jsr putc
        bcs @nx
        inc LLEN
@nx     lda RS
        and #$40
        beq @rd
@eof    lda LLEN
        beq @e2
        jsr endl                ; a last line without a CR
@e2     jsr CLRCHN
@cl     lda #2
        jsr CLOSE
        sec
        lda P3
        sbc #<TXT
        sta TLEN
        lda P3+1
        sbc #>TXT
        sta TLEN+1
        jmp reindex

; end a line of the file: a CR, unless MAXL lines are there already
endl    lda #0
        sta LLEN
        lda NLC
        cmp #MAXL
        bcs @d
        inc NLC
        lda #13
        ldy #0
        sta (P3),y
        jmp incp3
@d      rts

; A -> (P3)+ while P3 is below page LIMHI (carry set = no room; the last page is left for the CRs)
putc    ldx P3+1
        cpx LIMHI
        bcs putcr
        ldy #0
        sta (P3),y
incp3   inc P3
        bne putok
        inc P3+1
putok   clc
putcr   rts

; LSL/LSH/LLN, NL: start and length of every line of TXT (TLEN bytes); then parse
reindex lda #<TXT
        sta P4
        clc
        adc TLEN
        sta TEND
        lda #>TXT
        sta P4+1
        adc TLEN+1
        sta TEND+1
        lda #0
        sta NL
        sta LLEN
        jsr @mark
@l      lda P4
        cmp TEND
        bne @c
        lda P4+1
        cmp TEND+1
        bne @c
        jmp parse
@c      ldy #0
        lda (P4),y
        inc P4
        bne @i
        inc P4+1
@i      cmp #13
        beq @e
        inc LLEN
        jmp @l
@e      ldx NL
        cpx #MAXL
        bcs @s
        lda LS0
        sta LSL,x
        lda LS0+1
        sta LSH,x
        lda LLEN
        sta LLN,x
        inc NL
@s      lda #0
        sta LLEN
        jsr @mark
        jmp @l
@mark   lda P4
        sta LS0
        lda P4+1
        sta LS0+1
        rts

; The entries: lines that start with "/" after any spaces, "path[, color[, flashes]]"; the numbers are 1-3 digits after the last
; commas (spaces around them allowed), color taken AND 15, flashes at most 20, both 1 when missing. NE entries of at most MAXE:
; EPL/EPH/ELN the path (in TXT), EC, EF, EX the line.
parse   lda #0
        sta NE
        sta LI
@ln     lda LI
        cmp NL
        bcc @1
        rts
@1      lda NE
        cmp #MAXE
        bcc @2
        rts
@nxj    jmp @nx
@2      ldx LI
        lda LSL,x
        sta P2
        lda LSH,x
        sta P2+1
        lda LLN,x
        sta SLEN
        ldy #0
@sp     cpy SLEN
        bcs @nxj
        lda (P2),y
        cmp #' '
        bne @3
        iny
        bne @sp
@3      cmp #'/'
        bne @nxj
        sty T0                  ; the path starts here
        tya
        clc
        adc P2
        sta P2
        bcc @4
        inc P2+1
@4      lda SLEN
        sec
        sbc T0
        sta SLEN
        lda #1
        sta PC
        sta PF
        jsr trailnum
        bcs @tr
        lda V
        sta N1
        lda V+1
        sta N1+1
        jsr trailnum
        bcc @two
        lda N1                  ; one number: the color
        and #15
        sta PC
        jmp @tr
@two    lda V
        and #15
        sta PC
        lda N1+1
        bne @20
        lda N1
        cmp #21
        bcc @f
@20     lda #20
@f      sta PF
@tr     ldy SLEN                ; trailing spaces off
        beq @st
        dey
        lda (P2),y
        cmp #' '
        bne @st
        dec SLEN
        jmp @tr
@st     ldx NE
        lda P2
        sta EPL,x
        lda P2+1
        sta EPH,x
        lda SLEN
        sta ELN,x
        lda PC
        sta EC,x
        lda PF
        sta EF,x
        lda LI
        sta EX,x
        inc NE
@nx     inc LI
        jmp @ln

; the number after the last comma of (P2)/SLEN: V, and SLEN cut before that comma; carry set = none
trailnum
        ldy SLEN
        jmp @f
@no0    sec
        rts
@f      tya
        beq @no0
        dey
        lda (P2),y
        cmp #','
        bne @f
        sty CI
        iny
@s      cpy SLEN
        bcs @no0
        lda (P2),y
        cmp #' '
        bne @e0
        iny
        bne @s
@e0     sty T0                  ; first non-space
        ldy SLEN
@e      dey
        lda (P2),y
        cmp #' '
        beq @e
        iny
        sty T1                  ; after the last non-space
        tya
        sec
        sbc T0
        cmp #4
        bcs @no
        lda #0
        sta V
        sta V+1
        ldy T0
@d      lda (P2),y
        sec
        sbc #'0'
        cmp #10
        bcs @no
        sta T2
        lda V                   ; V = V * 10 + digit
        sta T3
        lda V+1
        sta T4
        asl V
        rol V+1
        asl V
        rol V+1
        clc
        lda V
        adc T3
        sta V
        lda V+1
        adc T4
        sta V+1
        asl V
        rol V+1
        clc
        lda V
        adc T2
        sta V
        bcc @n
        inc V+1
@n      iny
        cpy T1
        bcc @d
        lda CI
        sta SLEN
        clc
        rts
@no     sec
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; Change the file: OPK 0 = line OPA becomes LINEB, 1 = line OPA goes, 2 = lines OPA and OPB swap, 3 = LINEB is added at the end.
; The new text is built in NEWB and copied to TXT, then indexed and parsed. Carry set (nothing changed) when it does not fit.
rebuild lda #<NEWB
        sta P3
        lda #>NEWB
        sta P3+1
        lda #(>NEWB)+15
        sta LIMHI
        lda #0
        sta OVF
        sta LI
@l      lda LI
        cmp NL
        bcs @end
        ldx LI
        lda OPK
        cmp #2
        bne @n2
        cpx OPA
        bne @a
        ldx OPB
        jmp @cp
@a      cpx OPB
        bne @cp
        ldx OPA
        jmp @cp
@n2     cpx OPA
        bne @cp
        cmp #1
        beq @nx
        cmp #0
        bne @cp
        jsr emitlb
        jmp @nx
@cp     jsr emitln
@nx     inc LI
        jmp @l
@end    lda OPK
        cmp #3
        bne @ck
        jsr emitlb
@ck     lda OVF
        beq @ok
        sec
        rts
@ok     sec
        lda P3
        sbc #<NEWB
        sta TLEN
        lda P3+1
        sbc #>NEWB
        sta TLEN+1
        lda #<NEWB
        sta P4
        lda #>NEWB
        sta P4+1
        lda #<TXT
        sta P3
        lda #>TXT
        sta P3+1
        ldx #16                 ; the whole 4 KB
        ldy #0
@m      lda (P4),y
        sta (P3),y
        iny
        bne @m
        inc P4+1
        inc P3+1
        dex
        bne @m
        jsr reindex
        clc
        rts

; line X of TXT / LINEB, and a CR, to (P3)
emitln  lda LSL,x
        sta P4
        lda LSH,x
        sta P4+1
        lda LLN,x
        sta SLEN2
        jmp emit
emitlb  lda #<LINEB
        sta P4
        lda #>LINEB
        sta P4+1
        lda LBLEN
        sta SLEN2
emit    lda #0
        sta IX
@l      ldy IX
        cpy SLEN2
        bcs @cr
        lda (P4),y
        jsr putc
        bcc @n
        lda #1
        sta OVF
@n      inc IX
        jmp @l
@cr     lda #13
        ldy #0
        sta (P3),y
        jmp incp3

; LINEB = "path, color, flashes" from (P2)/SLEN, PC, PF
mkline  lda #OUTLINE
        sta OUTM
        lda #0
        sta LBLEN
        jsr fmem
        jsr pstr
        !pet ", ", 0
        lda PC
        ldx #0
        jsr fnum
        jsr pstr
        !pet ", ", 0
        lda PF
        ldx #0
        jsr fnum
        jmp scr

; Write TXT to the shown unit's "!CONFIG,W" (refused over SAVEMAX bytes) and say how it went.
save    lda TLEN+1
        cmp #>(SAVEMAX+1)
        bcc @ok
        bne @long
        lda TLEN
        cmp #<(SAVEMAX+1)
        bcc @ok
@long   jsr msgbeg
        jsr pstr
        !pet "List too long to save", 0
        jsr fpad
        rts
@ok     jsr cmdbeg
        jsr pstr
        !tx "!CONFIG,W", 0
        jsr scr
        lda #2
        ldx DV
        ldy #2
        jsr openn
        jsr rdel
        lda ENUM
        beq @w
        lda #2
        jsr CLOSE
        jsr msgbeg
        jsr pstr
        !pet "Cannot write:", 0
        jsr pemsg
        jmp fpad
@w      ldx #2
        jsr CHKOUT
        lda #<TXT
        sta P4
        lda #>TXT
        sta P4+1
        lda #0
        sta CNT
        sta CNT+1
@l      lda CNT
        cmp TLEN
        bne @c
        lda CNT+1
        cmp TLEN+1
        beq @e
@c      ldy #0
        lda (P4),y
        jsr CHROUT
        inc P4
        bne @i
        inc P4+1
@i      inc CNT
        bne @l
        inc CNT+1
        jmp @l
@e      jsr CLRCHN
        lda #2
        jsr CLOSE
        jsr rdel
        lda ENUM
        bne @we
        jsr msgbeg
        jsr pstr
        !pet "Saved", 0
        jsr fpad
        rts
@we     jsr msgbeg
        jsr pstr
        !pet "Write error ", 0
        lda ENUM
        ldx #0
        jsr fnum
        jmp fpad

; ---------------------------------------------------------------------------------------------------------------------------------
; The screen: row 0 title, 1-2 the units, 3 the list header, 4-19 the list, 20 messages, 21-23 the keys.
drawall jsr scr
        jsr pstr                ; the menu clears in the name color
        !pet C_NAME, 147, 0
        ldx #0
        jsr rowbeg
        jsr pstr
        !pet C_BAN, 18, "             TDrive Config", 0
        jsr fpad
        jsr pstr
        !pet 146, 0
        lda #$A0                ; the banner's last column straight into the screen: printing it would link rows 0 and 1
        sta $0400+39
        lda THEME+2
        sta $D800+39
drawmid jsr drawdrv
        jsr drawlh
        jsr drawlist
        ldx #21
        jsr rowbeg
        jsr pstr
        !by 18, C_OPT : !pet "Return" : !by 146, C_SRC : !pet " Mount "
        !by 18, C_OPT : !pet "B" : !by 146, C_SRC : !pet " Browse+Mount "
        !by 18, C_OPT : !pet "U" : !by 146, C_SRC : !pet " Unmount"
        !by 0
        jsr fpad
        ldx #22
        jsr rowbeg
        jsr pstr
        !by 18, C_OPT : !pet "A" : !by 146, C_SRC : !pet " Add "
        !by 18, C_OPT : !pet "E" : !by 146, C_SRC : !pet " Edit "
        !by 18, C_OPT : !pet "D" : !by 146, C_SRC : !pet " Delete "
        !by 18, C_OPT : !pet "+/-" : !by 146, C_SRC : !pet " Reorder"
        !by 0
        jsr fpad
        ldx #23
        jsr rowbeg
        jsr pstr
        !by 18, C_OPT : !pet "T" : !by 146, C_SRC : !pet " SD/USB list "
        !by 18, C_OPT : !pet "R" : !by 146, C_SRC : !pet " Refresh "
        !by 18, C_OPT : !pet "Q" : !by 146, C_SRC : !pet " Exit to menu"
        !by 0
        jmp fpad

; "SD  8: /path" / "USB 9: (Nothing mounted)" / "USB  : No drive", the shown unit's name in reverse
drawdrv lda #0
        sta U
@u      ldx U
        inx
        jsr atrow
        lda #255
        sta FLDN
        jsr pstr
        !pet C_SRC, 0
        lda U
        cmp UN
        bne @n
        jsr pstr
        !pet 18, C_OPT, 0
@n      lda U
        bne @usb
        jsr pstr
        !pet "SD ", 0
        jmp @num
@usb    jsr pstr
        !pet "USB", 0
@num    ldx U
        lda DEVS,x
        bne @d
        jsr pstr
        !pet "  : ", 0
        jmp @m
@d      ldx #2
        jsr fnum
        jsr pstr
        !pet ": ", 0
@m      jsr pstr
        !pet 146, C_NAME, 0
        lda #32
        sta FLDN
        ldx U
        lda DEVS,x
        bne @h
        jsr pstr
        !pet "No drive", 0
        jmp @p
@h      lda MPL,x
        bne @mp
        jsr pstr
        !pet "(Nothing mounted)", 0
        jmp @p
@mp     jsr mpptr
        lda #32
        jsr fpath
@p      jsr fpad
        inc U
        lda U
        cmp #2
        bcs @x
        jmp @u
@x      rts

drawlh  ldx #3
        jsr rowbeg
        jsr pstr
        !pet C_SRC, "List: ", 0
        lda UN
        bne @u
        jsr pstr
        !pet "SD", 0
        jmp @n
@u      jsr pstr
        !pet "USB", 0
@n      jsr pstr
        !pet " (", 0
        lda NE
        ldx #0
        jsr fnum
        jsr pstr
        !pet " entries)", 0
@sp     lda FLDN                ; the Alt button gesture for this list, at the right end
        cmp #12
        bcc @alt
        lda #' '
        jsr fch
        jmp @sp
@alt    jsr pstr
        !pet C_TIME, 0
        lda UN
        bne @a2
        jsr pstr
        !pet "Alt: 1 tap", 0
        jmp @e
@a2     jsr pstr
        !pet "Alt: 2 taps", 0
@e      jmp fpad

drawlist
        lda #0
        sta R
@r      lda TP
        clc
        adc R
        sta KK
        jsr drawrow
        inc R
        lda R
        cmp #VR
        bcc @r
        rts

; list entry KK on its row: ">" on the cursor, "*" when mounted, number, path (reverse on the cursor), color, a block in it, flashes
drawrow lda KK
        sec
        sbc TP
        clc
        adc #4
        tax
        jsr rowbeg
        lda KK
        cmp NE
        bcc @e
        jmp fpad
@e      lda #255
        sta FLDN
        jsr pstr
        !pet C_OPT, 0
        lda #' '
        ldx KK
        cpx CU
        bne @c
        lda #'>'
@c      jsr fch
        jsr mstar
        jsr fch
        jsr pstr
        !pet C_SRC, 0
        lda KK
        clc
        adc #1
        ldx #3
        jsr fnum
        jsr pstr
        !pet " ", C_NAME, 0
        lda KK
        cmp CU
        bne @p
        lda #18
        jsr fch
@p      ldx KK
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        lda ELN,x
        sta SLEN
        lda #22
        jsr fpath
        lda #255
        sta FLDN
        jsr pstr
        !pet 146, C_SRC, " ", 0
        ldx KK
        lda EC,x
        ldx #3
        jsr fnum
        jsr pstr
        !pet " ", 18, 0
        ldx KK
        lda EC,x                ; the block in the entry's color
        sta TXTCOL
        jsr pstr
        !pet "  ", 146, C_SRC, 0
        ldx KK
        lda EF,x
        ldx #3
        jsr fnum
        lda #'X'
        jmp fch

THEME   !by 0, 4, 4, 10, 7, 14, 13 ; the TR menu's color references (its defaults until "!STATE" gives them): see C_BAN

; A = '*' when entry KK is the image mounted on the shown unit, else ' '
mstar   ldx UN
        jsr mpptr
        lda P2
        sta P4
        lda P2+1
        sta P4+1
        ldx KK
        lda ELN,x
        cmp SLEN
        bne @no
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        ldy SLEN
@c      dey
        cpy #255
        beq @yes
        lda (P2),y
        cmp (P4),y
        beq @c
@no     lda #' '
        rts
@yes    lda #'*'
        rts

; keep the cursor on the screen
fixtp   lda CU
        cmp TP
        bcs @a
        sta TP
@a      lda TP
        clc
        adc #VR
        sta T0
        lda CU
        cmp T0
        bcc @r
        sec
        sbc #VR-1
        sta TP
@r      rts

; keep the cursor in the list
clampcu lda CU
        cmp NE
        bcc @r
        ldx NE
        dex
        bpl @s
        ldx #0
@s      stx CU
@r      rts

; ---------------------------------------------------------------------------------------------------------------------------------
; The keys.
k_up    lda CU
        beq @r
        dec CU
        lda CU
        cmp TP
        bcs @two
        sta TP
        jmp drawlist
@two    sta KK
        inc KK
        jsr drawrow
        lda CU
        sta KK
        jmp drawrow
@r      rts

k_down  lda CU
        clc
        adc #1
        cmp NE
        bcs @r
        sta CU
        lda TP
        clc
        adc #VR
        cmp CU
        beq @sc
        bcc @sc
        lda CU
        sta KK
        dec KK
        jsr drawrow
        lda CU
        sta KK
        jmp drawrow
@sc     lda CU
        sec
        sbc #VR-1
        sta TP
        jmp drawlist
@r      rts

; RETURN: mount the entry
k_ret   lda NE
        bne @go
        rts
@go     ldx CU
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        lda ELN,x
        sta SLEN
; "CD:" + (P2)/SLEN on the shown unit
mountp  jsr cmdbeg
        jsr pstr
        !tx "CD:", 0
        jsr fmem
        jsr sendcmd
        lda ENUM
        bne @e
        jsr msgbeg
        jsr pstr
        !pet "Mounted", 0
        jsr fpad
        jmp @d
@e      jsr perr
@d      jsr readstate
        jmp drawmid

; B: browse the card, mount the image picked
k_brow  jsr browse
        lda BR
        bne @m
        jmp drawall
@m      jsr pathbp
        jmp mountp

; U: unmount
k_unm   ldx UN
        lda MPL,x
        bne @u
        jsr msgbeg
        jsr pstr
        !pet "Nothing mounted", 0
        jsr fpad
        rts
@u      jsr cmdbeg
        jsr pstr
        !tx "CD:", LARROW, 0
        jsr sendcmd
        lda ENUM
        bne @e
        jsr msgbeg
        jsr pstr
        !pet "Unmounted", 0
        jsr fpad
        jmp @d
@e      jsr msgbeg
        jsr pstr
        !pet "Error ", 0
        lda ENUM
        ldx #0
        jsr fnum
        jsr fpad
@d      jsr readstate
        jmp drawmid

; A: add an entry (browse, color, flashes)
k_add   lda NE
        cmp #MAXE
        bcs @full
        lda NL
        cmp #MAXL-1
        bcc @ok
@full   jsr msgbeg
        jsr pstr
        !pet "List is full", 0
        jsr fpad
        rts
@ok     jsr browse
        lda BR
        bne @c
        jmp drawall
@c      lda #15
        sta NMAX
        lda #1
        ldx #<lbcol
        ldy #>lbcol
        jsr asknum
        bcs @cn
        sta PC
        lda #20
        sta NMAX
        lda #1
        ldx #<lbfl
        ldy #>lbfl
        jsr asknum
        bcs @cn
        sta PF
        jsr pathbp
        jsr mkline
        lda #3
        sta OPK
        lda #255
        sta OPA
        jsr rebuild
        bcs @long
        ldx NE
        dex
        stx CU
        jsr fixtp
        jsr drawall
        jmp save
@cn     jmp drawall
@long   jmp toolong

; E: edit the entry (path, "?" browses for it; color; flashes)
k_edit  lda NE
        bne @go
        rts
@go     ldx CU
        lda EPL,x
        sta P2
        lda EPH,x
        sta P2+1
        lda ELN,x
        cmp #255
        bcc @l
        lda #254
@l      sta EDLEN
        ldy #0
@cp     cpy EDLEN
        bcs @cd
        lda (P2),y
        sta EDBUF,y
        iny
        bne @cp
@cd     lda #78
        ldx #<lbpath
        ldy #>lbpath
        jsr edline
        bcc @got
        jmp drawmid
@got    lda EDLEN
        cmp #1
        bne @typed
        lda EDBUF
        cmp #'?'
        bne @typed
        jsr browse
        lda BR
        bne @chk
        jmp drawall
@typed  ldy #0
@t      cpy EDLEN
        bcs @td
        lda EDBUF,y
        sta PATHB,y
        iny
        bne @t
@td     sty PATHBL
@chk    lda PATHBL
        beq @bad
        lda PATHB
        cmp #'/'
        beq @num
@bad    jsr drawmid
        jsr msgbeg
        jsr pstr
        !pet "Path must start with /", 0
        jsr fpad
        rts
@num    lda #15
        sta NMAX
        ldx CU
        lda EC,x
        ldx #<lbcol
        ldy #>lbcol
        jsr asknum
        bcs @cn
        sta PC
        lda #20
        sta NMAX
        ldx CU
        lda EF,x
        ldx #<lbfl
        ldy #>lbfl
        jsr asknum
        bcs @cn
        sta PF
        jsr pathbp
        jsr mkline
        lda #0
        sta OPK
        ldx CU
        lda EX,x
        sta OPA
        jsr rebuild
        bcs toolong
        jsr drawall
        jmp save
@cn     jmp drawall

toolong jsr drawall
        jsr msgbeg
        jsr pstr
        !pet "List too long", 0
        jsr fpad
        rts

; D: delete the entry (asks)
k_del   lda NE
        bne @go
        rts
@go     jsr msgbeg
        jsr pstr
        !pet "Delete entry ", 0
        lda CU
        clc
        adc #1
        ldx #0
        jsr fnum
        jsr pstr
        !pet "? (y/n)", 0
        jsr fpad
        jsr getkey
        cmp #'Y'
        beq @y
        cmp #'Y'+128
        beq @y
        jsr msgbeg
        jmp fpad
@y      lda #1
        sta OPK
        ldx CU
        lda EX,x
        sta OPA
        jsr rebuild
        jsr clampcu
        jmp moved

; +: move the entry down, -: up
k_plus  lda CU
        clc
        adc #1
        cmp NE
        bcc @go
        rts
@go     tax
        lda EX,x
        sta OPB
        ldx CU
        lda EX,x
        sta OPA
        lda #2
        sta OPK
        jsr rebuild
        inc CU
moved   jsr fixtp
        jsr drawmid
        jmp save

k_minus lda CU
        bne @go
        rts
@go     tax
        dex
        lda EX,x
        sta OPB
        ldx CU
        lda EX,x
        sta OPA
        lda #2
        sta OPK
        jsr rebuild
        dec CU
        jmp moved

; T: the other unit's list
k_tog   lda UN
        eor #1
        tax
        lda DEVS,x
        bne @ok
        jsr msgbeg
        jsr pstr
        !pet "No drive on that unit", 0
        jsr fpad
        rts
@ok     stx UN
        jsr setunit
        jsr readcfg
        lda #0
        sta CU
        sta TP
        jmp drawmid

; R: read everything again (the Alt button may have switched the disk)
k_ref   jsr readstate
        jsr setunit
        jsr readcfg
        jsr clampcu
        jsr fixtp
        jsr drawmid
        jsr msgbeg
        jsr pstr
        !pet "Refreshed", 0
        jsr fpad
        rts

pathbp  lda #<PATHB
        sta P2
        lda #>PATHB
        sta P2+1
        lda PATHBL
        sta SLEN
        rts

lbcol   !pet "Color 0-15: ", 0
lbfl    !pet "Flashes 0-20: ", 0
lbpath  !pet "Path(?=browse): ", 0

; ---------------------------------------------------------------------------------------------------------------------------------
; Line editor on row 23: label at X/Y (zero terminated), EDBUF/EDLEN the text, at most A characters typed (a longer text can still be
; shortened). RETURN: carry clear; left arrow: carry set. Typed: codes 32-94.
edline  sta ML
        stx LBP
        sty LBP+1
@show   ldx #23
        jsr atrow
        lda #255
        sta FLDN
        lda #C_SRC
        jsr fch
        lda LBP
        sta P2
        lda LBP+1
        sta P2+1
        ldy #0
@lb     lda (P2),y
        beq @le
        jsr fch
        iny
        bne @lb
@le     sty T0
        lda #C_NAME
        jsr fch
        lda #38
        sec
        sbc T0
        sta T1
        ldx EDLEN
        lda #CURSOR
        sta EDBUF,x
        inx
        stx SLEN
        lda #<EDBUF
        sta P2
        lda #>EDBUF
        sta P2+1
        lda T1
        jsr fpath
@k      jsr getkey
        cmp #13
        beq @ok
        cmp #LARROW
        beq @cn
        cmp #20
        bne @ch
        lda EDLEN
        beq @k
        dec EDLEN
        jmp @show
@ch     cmp #32
        bcc @k
        cmp #LARROW
        bcs @k
        ldx EDLEN
        cpx ML
        bcs @k
        sta EDBUF,x
        inc EDLEN
        jmp @show
@ok     clc
        rts
@cn     sec
        rts

; Ask for a number with label X/Y, default A, at most NMAX: A = the number (the default when it is not one or too big), carry set = canceled.
asknum  sta DEF
        stx LBP2
        sty LBP2+1
        lda #OUTLINE
        sta OUTM
        lda #0
        sta LBLEN
        lda DEF
        ldx #0
        jsr fnum
        jsr scr
        ldx #0
@c      cpx LBLEN
        bcs @d
        lda LINEB,x
        sta EDBUF,x
        inx
        bne @c
@d      stx EDLEN
        lda #2
        ldx LBP2
        ldy LBP2+1
        jsr edline
        bcs @r
        jsr edval
        bcs @def
        cmp NMAX
        bcc @ok
        beq @ok
@def    lda DEF
@ok     clc
@r      rts

; EDBUF as a number (leading spaces, then digits up to the first other character; empty = 0): A, carry set if it starts with '-'
edval   ldy #0
        lda #0
        sta V
@s      cpy EDLEN
        bcs @d
        lda EDBUF,y
        cmp #' '
        bne @g
        iny
        bne @s
@g      cmp #'-'
        beq @bad
@l      cpy EDLEN
        bcs @d
        lda EDBUF,y
        sec
        sbc #'0'
        cmp #10
        bcs @d
        sta T2
        lda V
        asl
        asl
        adc V
        asl
        adc T2
        sta V
        iny
        bne @l
@d      lda V
        clc
        rts
@bad    sec
        rts

; ---------------------------------------------------------------------------------------------------------------------------------
; The browser: folders and .D64/.D71/.D81 of the shown unit, from "$/folder/" listings (a listing by path leaves the mount alone).
; RETURN opens a folder (".." goes up) or picks an image: BR = 1 and PATHB / PATHBL its path. Left arrow: BR = 0.
browse  lda #'/'
        sta BP
        lda #1
        sta BPLN
        lda #0
        sta BR
@new    lda #0
        sta BQ
        sta BW
        jsr blist
@draw   jsr bdraw
@key    jsr getkey
        cmp #LARROW
        bne @1
        rts
@1      cmp #145
        bne @2
        lda BQ
        beq @key
        dec BQ
        jmp @mv
@2      cmp #17
        bne @3
        lda BQ
        clc
        adc #1
        cmp BK
        bcs @key
        sta BQ
        jmp @mv
@3      cmp #13
        bne @key
        lda BK
        beq @key
        lda BQ
        bne @in
        lda BPLN
        cmp #2
        bcc @in                 ; the root has no ".."
        ldy BPLN                ; "..": cut at the last "/"
@sl     dey
        lda BP,y
        cmp #'/'
        bne @sl
        sty BPLN
        tya
        bne @new
        lda #1                  ; up to "/" itself
        sta BPLN
        jmp @new
@in     jsr mkjn
        bcs @key
        ldx BQ
        lda BTY,x
        bne @img
        ldy #0                  ; a folder: BP = PATHB
@c      lda PATHB,y
        sta BP,y
        iny
        cpy PATHBL
        bcc @c
        sty BPLN
        jmp @new
@img    lda #1
        sta BR
        rts
@mv     lda BQ
        cmp BW
        bcs @m1
        sta BW
@m1     lda BW
        clc
        adc #VR
        sta T0
        lda BQ
        cmp T0
        bcc @m2
        sec
        sbc #VR-1
        sta BW
@m2     jmp @draw

; PATHB = BP + "/" + name BQ (no second "/" at the root); carry set (and a message) when longer than BPMAX
mkjn    lda BQ
        jsr bname
        lda BPLN
        clc
        adc SLEN
        cmp #BPMAX
        bcs @long
        ldy #0
@c      lda BP,y
        sta PATHB,y
        iny
        cpy BPLN
        bcc @c
        cpy #2
        bcc @r
        lda #'/'
        sta PATHB,y
        iny
@r      tya
        tax
        ldy #0
@n      cpy SLEN
        bcs @d
        lda (P2),y
        sta PATHB,x
        inx
        iny
        bne @n
@d      stx PATHBL
        clc
        rts
@long   jsr msgbeg
        jsr pstr
        !pet "Path too long", 0
        jsr fpad
        sec
        rts

; P2 = the name of browser entry A (16-byte slots), SLEN its length
bname   tax
        lda BNL,x
        sta SLEN
        stx P2
        lda #0
        sta P2+1
        ldy #4
@s      asl P2
        rol P2+1
        dey
        bne @s
        clc
        lda P2
        adc #<BNAMES
        sta P2
        lda P2+1
        adc #>BNAMES
        sta P2+1
        rts

; read the listing of BP into BNAMES/BNL/BTY, BK entries (first ".." below the root)
blist   lda #0
        sta BK
        lda BPLN
        cmp #2
        bcc @o
        lda #0
        jsr bname
        lda #'.'
        ldy #0
        sta (P2),y
        iny
        sta (P2),y
        lda #2
        sta BNL
        lda #0
        sta BTY
        inc BK
@o      jsr cmdbeg
        jsr pstr
        !tx "$", 0
        lda #<BP
        sta P2
        lda #>BP
        sta P2+1
        lda BPLN
        sta SLEN
        jsr fmem
        ldx BPLN
        lda BP-1,x
        cmp #'/'
        beq @s
        lda #'/'
        jsr fch
@s      jsr scr
        jsr fopen               ; in blocks when the drive answers that
        bcc @go
        lda ENUM
        bne @cant               ; it refused, with a reason
        lda #1
        ldx DV
        ldy #0
        jsr openn
        jsr rdel
        lda ENUM
        beq @rd
@cant   lda #1
        jsr CLOSE
        jsr msgbeg
        jsr pstr
        !pet "Cannot list: ", 0
        jsr pemsg
        jmp fpad
@rd     ldx #1
        jsr CHKIN
@go     lda #0
        sta STATUS
        jsr rdb                 ; load address
        jsr rdb
@line   jsr rdb                 ; link
        sta T0
        jsr rdb
        ora T0
        sta T0
        lda STATUS
        and #$BF
        bne @end
        lda T0
        beq @end
        jsr rdb                 ; line number (blocks)
        jsr rdb
        lda #0
        sta LLEN
@ch     jsr rdb
        sta CH
        lda STATUS
        sta RS
        and #$BF
        bne @end
        lda CH
        beq @eol
        ldy LLEN
        cpy #60
        bcs @nx
        sta LINE,y
        inc LLEN
@nx     lda RS
        and #$40
        beq @ch
@eol    jsr bentry
        lda RS
        and #$40
        beq @line
@end    jsr CLRCHN
        lda #1
        jmp CLOSE

; one listing line in LINE/LLEN: the name between the first two quotes (trailing spaces off), kept when the type after it is DIR/D64/D71/D81
bentry  ldy LLEN
        lda #0
        sta LINE,y
        sta Q1
        ldy #0
@q      cpy LLEN
        bcs @no
        lda LINE,y
        iny
        cmp #34
        bne @q
        ldx Q1
        bne @two
        sty Q1                  ; the name starts here
        jmp @q
@two    dey
        sty Q2                  ; the closing quote
        tya
        sec
        sbc Q1
        sta NLN
@tr     lda NLN
        beq @ty
        clc
        adc Q1
        tay
        lda LINE-1,y
        cmp #' '
        bne @ty
        dec NLN
        jmp @tr
@ty     ldy Q2
        iny
@sp     lda LINE,y
        cmp #' '
        bne @t0
        iny
        bne @sp
@t0     sty T0
        ldx #0
        stx T1
@tc     ldy T0
        lda LINE,y
        cmp types,x
        bne @tn
        lda LINE+1,y
        cmp types+1,x
        bne @tn
        lda LINE+2,y
        cmp types+2,x
        beq @hit
@tn     inc T1
        inx
        inx
        inx
        cpx #12
        bcc @tc
@no     rts
@hit    lda BK
        cmp #MAXB
        bcs @no
        jsr bname
        ldx BK
        lda T1
        sta BTY,x
        lda NLN
        cmp #17
        bcc @l
        lda #16
@l      sta BNL,x
        sta SLEN
        ldy #0
@cp     cpy SLEN
        bcs @d
        tya
        clc
        adc Q1
        tax
        lda LINE,x
        sta (P2),y
        iny
        bne @cp
@d      inc BK
        rts

types   !tx "DIRD64D71D81"

; the browser screen: row 3 the folder, 4-19 the entries, 21 the keys
bdraw   ldx #3
        jsr rowbeg
        jsr pstr
        !pet C_SRC, "Browse: ", C_NAME, 0
        lda #<BP
        sta P2
        lda #>BP
        sta P2+1
        lda BPLN
        sta SLEN
        jsr fmem
        jsr fpad
        lda #0
        sta R
@r      lda BW
        clc
        adc R
        sta KK
        lda R
        clc
        adc #4
        tax
        jsr rowbeg
        lda KK
        cmp BK
        bcc @e
        jsr fpad
        jmp @nx
@e      lda #255
        sta FLDN
        jsr pstr
        !pet C_OPT, 0
        lda #' '
        ldx KK
        cpx BQ
        bne @c
        lda #'>'
@c      jsr fch
        jsr pstr
        !pet C_NAME, 0
        lda KK
        cmp BQ
        bne @n
        lda #18
        jsr fch
@n      lda KK
        jsr bname
        lda #17
        jsr ffield
        lda #255
        sta FLDN
        jsr pstr
        !pet 146, C_SRC, " ", 0
        ldx KK
        lda BTY,x
        sta T0
        asl
        adc T0
        tax
        lda types,x
        jsr fchu
        lda types+1,x
        jsr fchu
        lda types+2,x
        jsr fchu
        lda #17
        sta FLDN
        jsr fpad
@nx     inc R
        lda R
        cmp #VR
        bcs @h
        jmp @r
@h      ldx #21
        jsr rowbeg
        jsr pstr
        !by 18, C_OPT : !pet "Return" : !by 146, C_SRC : !pet " Open/Select  "
        !by 18, C_OPT : !pet LARROW : !by 146, C_SRC : !pet " Cancel"
        !by 0
        jsr fpad
        ldx #22
        jsr rowbeg
        jsr fpad
        ldx #23
        jsr rowbeg
        jmp fpad

; ---------------------------------------------------------------------------------------------------------------------------------
!if * > $2000 { !error "the program runs into its BSS at $2000" }
; BSS from $2000: labels only, not part of the PRG
TXT     = $2000                 ; the file ($2000, page aligned: putc checks pages)
NEWB    = TXT + 4096            ; the file being rebuilt
MPB     = NEWB + 4096           ; the mounted images, 128 bytes per unit (page aligned: mpptr adds 128 to the low byte)
!if ((<TXT) | (<NEWB) | (<MPB)) <> 0 { !error "TXT, NEWB and MPB must be page aligned" }
BNAMES  = MPB + 256
BNL     = BNAMES + (MAXB*16)
BTY     = BNL + MAXB
LSL     = BTY + MAXB
LSH     = LSL + MAXL
LLN     = LSH + MAXL
EPL     = LLN + MAXL
EPH     = EPL + MAXE
ELN     = EPH + MAXE
EC      = ELN + MAXE
EF      = EC + MAXE
EX      = EF + MAXE
BP      = EX + MAXE
PATHB   = BP + (BPMAX+1)
EDBUF   = PATHB + 256
LINEB   = EDBUF + 257
CMD     = LINEB + 256
LINE    = CMD + 256
ERRBUF  = LINE + 64
DIGS    = ERRBUF + 48
DEVS    = DIGS + 3
MPL     = DEVS + 2
TLEN    = MPL + 2
TEND    = TLEN + 2
LS0     = TEND + 2
CNT     = LS0 + 2
V       = CNT + 2
N1      = V + 2
LBP     = N1 + 2
LBP2    = LBP + 2
OUTM    = LBP2 + 2
FLDN    = OUTM + 1
FCH     = FLDN + 1
FX      = FCH + 1
FY      = FX + 1
NW      = FY + 1
ND      = NW + 1
SLEN    = ND + 1
SLEN2   = SLEN + 1
LBLEN   = SLEN2 + 1
CMDLEN  = LBLEN + 1
OC      = CMDLEN + 1
OB      = OC + 1
OT      = OB + 1
UN      = OT + 1
U       = UN + 1
UOFS    = U + 1
DV      = UOFS + 1
EL      = DV + 1
NL      = EL + 1
NLC     = NL + 1
NE      = NLC + 1
LI      = NE + 1
LLEN    = LI + 1
LIMHI   = LLEN + 1
CH      = LIMHI + 1
RS      = CH + 1
EI      = RS + 1
ERRLEN  = EI + 1
ENUM    = ERRLEN + 1
TMP     = ENUM + 1
IX      = TMP + 1
CU      = IX + 1
TP      = CU + 1
KK      = TP + 1
R       = KK + 1
PC      = R + 1
PF      = PC + 1
CI      = PF + 1
T0      = CI + 1
T1      = T0 + 1
T2      = T1 + 1
T3      = T2 + 1
T4      = T3 + 1
OPK     = T4 + 1
OPA     = OPK + 1
OPB     = OPA + 1
OVF     = OPB + 1
ML      = OVF + 1
DEF     = ML + 1
NMAX    = DEF + 1
EDLEN   = NMAX + 1
PATHBL  = EDLEN + 1
BPLN    = PATHBL + 1
BQ      = BPLN + 1
BW      = BQ + 1
BK      = BW + 1
BR      = BK + 1
Q1      = BR + 1
Q2      = Q1 + 1
NLN     = Q2 + 1
FAST    = NLN + 1               ; $80: the listing comes from fget
FI      = FAST + 1              ; fget: next byte in the window,
FN      = FI + 1                ; bytes in it,
FEND    = FN + 1                ; $40 once the last block came
FGX     = FEND + 1
MST     = FGX + 1               ; mcall: the answer's ST
MW0     = MST + 1
MW1     = MW0 + 1
THN     = MW1 + 1               ; the colors being read (THEME takes them when all seven came)
!if THN + 7 > $5000 { !error "the BSS ends at $4FFF" }
