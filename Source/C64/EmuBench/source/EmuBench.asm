; EmuBench: how fast the TR+ runs BASIC on the Teensy, in BasicAccel/BasicEmu.h.
;
; Launch it with the Special IO set to Debug: IOH_Debug answers $DEF0-$DEFF (see its "emulator
; benchmark"). Each run holds this C64 while the TR+ reads its BASIC and KERNAL ROMs by DMA,
; boots them on the Teensy, runs SinePlot.bas there to the end, and writes what it took to
; $C000, 32 bytes a run: first with the ROM's float routines emulated, then with every one
; answered by BasicAccel/CbmFloat.h. Nothing of the run reaches this C64's screen; the bitmap's
; checksum is compared with what tools/basic-accel/emu_model.c draws on the same ROMs.

   !cpu 6510

CHROUT   = $ffd2
LINPRT   = $bdcd       ; prints A (high) / X (low) as an unsigned decimal

FTCommand = $def0      ; W: command, R: status
FTRoutine = $def1
FTIdent   = $deff
CmdRoutine = 3
StBusy     = 1
StDone     = 2
BenchFirst = 6         ; FTRoutine: 6 without the hooks, 7 with them

Results  = $c000
; each run's 32 bytes, as IOH_Debug's struct FTBench lays them out
RStatus  = 0
RMs      = 2
RSpeed   = 4
RSpeedH  = 6
RMipsH   = 7
RMips    = 8
RHooks   = 10
RBitmap  = 12
RBasic   = 16
RKernal  = 18
RStopPc  = 20
; RStatus, as IOH_Debug's FTBenchStatuses numbers them; 2 is a JAM or undocumented opcode
BenchTimeLimit = 1
BenchNoMemory  = 3

; EmuBenchChecksum of SinePlot's bitmap, as emu_model.c draws it on BASIC 901226-01 and
; KERNAL 901227-03, natively or emulated, hooked or not
Expected = $f4b7cd00

Ptr      = $fb

   * = $0801
   !word BasicEnd
   !word 2026
   !byte $9e
   !byte (SysAddress/1000)+$30
   !byte ((SysAddress/100)%10)+$30
   !byte ((SysAddress/10)%10)+$30
   !byte SysAddress%10+$30
   !byte $00
BasicEnd
   !word $0000

SysAddress
   lda FTIdent
   cmp #'F'
   beq +
   lda #<MsgNoTR
   ldy #>MsgNoTR
   jmp PrintMsg
+  lda #<MsgRunning
   ldy #>MsgRunning
   jsr PrintMsg

   ldx #63
   lda #$ff               ; a status no run writes
-  sta Results,x
   dex
   bpl -

   ldx #0
-  stx Run
   txa
   clc
   adc #BenchFirst
   sta FTRoutine
   lda #CmdRoutine
   sta FTCommand          ; held here until the run is done
   nop
   nop
--  lda FTCommand
   cmp #StBusy
   beq --
   ldx Run
   sta TRStatus,x
   inx
   cpx #2
   bne -

   lda #<MsgTitle
   ldy #>MsgTitle
   jsr PrintMsg
   lda #<MsgRoms
   ldy #>MsgRoms
   jsr PrintMsg
   lda Results+RBasic+1
   jsr PrintHex
   lda Results+RBasic
   jsr PrintHex
   lda #<MsgKernal
   ldy #>MsgKernal
   jsr PrintMsg
   lda Results+RKernal+1
   jsr PrintHex
   lda Results+RKernal
   jsr PrintHex

   ldx #0
-  stx Run
   jsr Report
   ldx Run
   inx
   cpx #2
   bne -
   lda #$0d
   jmp CHROUT

; one run's report; Run is 0 or 1
Report
   lda Run
   asl
   asl
   asl
   asl
   asl
   clc
   adc #<Results
   sta Ptr
   lda #>Results
   sta Ptr+1
   lda Run
   bne +
   lda #<MsgPlain
   ldy #>MsgPlain
   bne ++
+  lda #<MsgHooked
   ldy #>MsgHooked
++ jsr PrintMsg

   ldx Run
   lda TRStatus,x
   cmp #StDone
   beq +
   pha
   lda #<MsgTRStatus
   ldy #>MsgTRStatus
   jsr PrintMsg
   pla
   tax
   lda #0
   jmp LINPRT
+  ldy #RStatus
   lda (Ptr),y
   beq ReportDone
   cmp #BenchNoMemory
   bne +
   lda #<MsgNoMemory
   ldy #>MsgNoMemory
   jmp PrintMsg
+  cmp #BenchTimeLimit
   bne +
   lda #<MsgTimeLimit
   ldy #>MsgTimeLimit
   bne ++
+  lda #<MsgStopped
   ldy #>MsgStopped
++ jsr PrintMsg
   ldy #RStopPc+1
   lda (Ptr),y
   jsr PrintHex
   ldy #RStopPc
   lda (Ptr),y
   jmp PrintHex

ReportDone
   lda #<MsgTime
   ldy #>MsgTime
   ldx #RMs
   jsr PrintWord
   lda #<MsgSpeed
   ldy #>MsgSpeed
   ldx #RSpeed
   jsr PrintWord
   ldx #RSpeedH
   jsr PrintHundredths
   lda #<MsgMips
   ldy #>MsgMips
   ldx #RMips
   jsr PrintWord
   ldx #RMipsH
   jsr PrintHundredths
   lda #<MsgHooks
   ldy #>MsgHooks
   ldx #RHooks
   jsr PrintWord

   lda #<MsgBitmap
   ldy #>MsgBitmap
   jsr PrintMsg
   ldx #3
   ldy #RBitmap+3
-  lda (Ptr),y
   cmp ExpectedBytes,x
   bne +
   dey
   dex
   bpl -
   lda #<MsgSame
   ldy #>MsgSame
   jmp PrintMsg
+  lda #<MsgDiffers
   ldy #>MsgDiffers
   jsr PrintMsg
   ldy #RBitmap+3
-  lda (Ptr),y
   jsr PrintHex
   dey
   cpy #RBitmap-1
   bne -
   rts

; A/Y: label; X: offset of a 16-bit value in this run's results
PrintWord
   stx Field
   jsr PrintMsg
   ldy Field
   iny
   lda (Ptr),y
   pha
   dey
   lda (Ptr),y
   tax
   pla
   jmp LINPRT

; X: offset of a byte of hundredths (0-99), printed as .NN; not LINPRT, which leads with a space
PrintHundredths
   txa
   tay
   lda #'.'
   jsr CHROUT
   lda (Ptr),y
   ldx #'0'-1
   sec
-  inx                    ; X: the tens digit
   sbc #10
   bcs -
   adc #10+'0'            ; carry clear: the units digit
   pha
   txa
   jsr CHROUT
   pla
   jmp CHROUT

PrintHex
   pha
   lsr
   lsr
   lsr
   lsr
   jsr PrintNibble
   pla
   and #$0f
PrintNibble
   cmp #10
   bcc +
   adc #6
+  adc #$30
   jmp CHROUT

PrintMsg
   sta Ptr2
   sty Ptr2+1
   ldy #0
-  lda (Ptr2),y
   beq +
   jsr CHROUT
   iny
   bne -
+  rts

Ptr2 = $fd

ExpectedBytes !byte <Expected, >Expected, <(Expected >> 16), >(Expected >> 16)

MsgNoTR    !pet $0d, "no emulator benchmark at $def0.", $0d
           !pet "launch this with the special io set to", $0d
           !pet "debug, on a teensyrom+.", $0d, 0
MsgRunning !pet $0d, "emulator benchmark: two runs of the", $0d
           !pet "sine plot on the teensy, up to a minute", $0d
           !pet "each...", $0d, 0
MsgTitle   !pet $93, "emulator benchmark, sineplot.bas", $0d, 0
MsgRoms    !pet "roms read: basic $", 0
MsgKernal  !pet " kernal $", 0
MsgPlain   !pet $0d, $0d, "float routines emulated:", 0
MsgHooked  !pet $0d, $0d, "float routines answered in c:", 0
MsgTRStatus !pet $0d, " tr status ", 0
MsgStopped !pet $0d, " stopped at $", 0
MsgTimeLimit !pet $0d, " time limit at $", 0
MsgNoMemory !pet $0d, " no memory for the emulator", 0
MsgTime    !pet $0d, " run to done, ms ", 0
MsgSpeed   !pet $0d, " x a 1 mhz 6502  ", 0
MsgMips    !pet $0d, " mips            ", 0
MsgHooks   !pet $0d, " hook calls      ", 0
MsgBitmap  !pet $0d, " bitmap          ", 0
MsgSame    !pet "as expected", 0
MsgDiffers !pet "differs: $", 0

Run      !byte 0
Field    !byte 0
TRStatus !byte 0, 0
