; FloatHandoff: what answering a BASIC float routine from the cartridge costs on real hardware.
;
; Launch it with the Special IO set to Debug: IOH_Debug answers $DEF0-$DEFF (see its "float
; hand-off test"). With the screen blanked and IRQs off, CIA2 timer A, which counts on while
; the TR holds the CPU, times:
;   - a ping the TR answers in thread mode while the C64 polls for it (no hold),
;   - a ping that holds the C64 until thread mode lets it go (the TR+ hand-off itself),
;   - for FADDT, FSUBT, FMULTT, FDIVT, QINT and AYINT, 256 operand pairs each: the ROM routine
;     natively, then the same operands answered by the TR (DMA read, the C port, DMA write).
; Each TR answer is compared with the ROM's on every byte of $22-$70 and on the registers and
; flags the port says it determined. All counts are C64 cycles, less the timing's own cost.

   !cpu 6510

CHROUT   = $ffd2
LINPRT   = $bdcd       ; prints A (high) / X (low) as an unsigned decimal
MOVFM    = $bba2       ; FAC = the float at A/Y
CONUPK   = $ba8c       ; ARG = the float at A/Y, $6F = the sign compare, A = $61

FTCommand = $def0      ; W: command, R: status
FTRoutine = $def1
FTResult  = $def2      ; A, X, Y, P, Set lo, Set hi
FTLatency = $def8      ; nS, lo/hi
FTCompute = $defa
FTWork    = $defc
FTIdent   = $deff

CmdPingPoll = 1
CmdPingHalt = 2
CmdRoutine  = 3
StBusy      = 1
StDone      = 2

CIA2TALo = $dd04
CIA2TAHi = $dd05
CIA2CRA  = $dd0e

TabA     = $3000       ; 256 floats, 5 bytes each
TabB     = TabA + 256 * 5
ZpFirst  = $22         ; the zero page compared: $22-$70
ZpLen    = $70 - $22 + 1

PtrA     = $fb
PtrB     = $fd

; result slots: 0 ping poll, 1 ping halt, then per routine R (0-5): native 2+R, TR 8+R,
; latency 14+R, compute 20+R, work 26+R
Slots    = 32

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

   jsr MakeTables
   jsr ClearStats
   sei
   lda $d011
   sta SaveD011
   and #$ef
   sta $d011
   jsr WaitFrame

   lda #0                 ; the timing's own cost
   sta OverLo
   sta OverHi
   jsr TimeStart
   lda #0
   sta CIA2CRA
   jsr TimeRead
   lda ValLo
   sta OverLo
   lda ValHi
   sta OverHi

   ldy #0                 ; pings
-  sty Count
   jsr TimeStart
   lda #CmdPingPoll
   sta FTCommand
--  lda FTCommand
   cmp #StBusy
   beq --
   lda #0
   sta CIA2CRA
   jsr TimeRead
   ldx #0
   jsr Accum
   jsr TimeStart
   lda #CmdPingHalt
   sta FTCommand
   lda #0
   sta CIA2CRA
   jsr TimeRead
   ldx #1
   jsr Accum
   lda FTCommand
   cmp #StDone
   beq +
   inc PingErrors
+  ldy Count
   iny
   bne -

   lda #0                 ; the routines
   sta Routine
--- lda Routine
   asl
   tax
   lda Entries,x
   sta Native+1
   lda Entries+1,x
   sta Native+2
   jsr Rewind
   lda #0
   sta Count
-  jsr OneCase
   clc
   lda PtrA
   adc #5
   sta PtrA
   bcc +
   inc PtrA+1
+  clc
   lda PtrB
   adc #5
   sta PtrB
   bcc +
   inc PtrB+1
+  inc Count
   bne -
   inc Routine
   lda Routine
   cmp #6
   bne ---

   lda SaveD011
   sta $d011
   cli
   jmp Report

; One operand pair for Routine: natively, then by the TR, then compared
OneCase
   lda PtrA
   ldy PtrA+1
   jsr MOVFM
   lda PtrB
   ldy PtrB+1
   jsr CONUPK
   ldx #ZpLen-1
-  lda ZpFirst,x
   sta Snap,x
   dex
   bpl -

   jsr TimeStart
   lda $61
Native
   jsr $ffff
   php
   sta NatA
   lda #0
   sta CIA2CRA
   stx NatX
   sty NatY
   pla
   sta NatP
   jsr TimeRead
   lda Routine
   clc
   adc #2
   tax
   jsr Accum

   ldx #ZpLen-1
-  lda ZpFirst,x
   sta NatZp,x
   lda Snap,x
   sta ZpFirst,x
   dex
   bpl -

   lda Routine
   sta FTRoutine
   jsr TimeStart
   lda #CmdRoutine
   sta FTCommand
   lda #0
   sta CIA2CRA
   jsr TimeRead
   lda Routine
   clc
   adc #8
   tax
   jsr Accum

   lda FTCommand
   cmp #StDone
   bne Wrong
   ldx #ZpLen-1
-  lda ZpFirst,x
   cmp NatZp,x
   bne Wrong
   dex
   bpl -
   lda FTResult+5         ; Set hi: A, X, Y
   lsr
   bcc +
   ldx FTResult
   cpx NatA
   bne Wrong
+  lsr
   bcc +
   ldx FTResult+1
   cpx NatX
   bne Wrong
+  lsr
   bcc +
   ldx FTResult+2
   cpx NatY
   bne Wrong
+  lda FTResult+3         ; the flags it determined: N V Z C
   eor NatP
   and FTResult+4
   and #$c3
   bne Wrong
   beq Timings
Wrong
   ldx Routine
   inc Errors,x
   bne Timings
   dec Errors,x           ; stays at 255
Timings
   lda Routine
   clc
   adc #14
   ldy #0
   jsr AccumTR
   lda Routine
   clc
   adc #20
   ldy #2
   jsr AccumTR
   lda Routine
   clc
   adc #26
   ldy #4
; A: slot, Y: offset of the TR's nS timing from FTLatency
AccumTR
   tax
   lda FTLatency,y
   sta ValLo
   lda FTLatency+1,y
   sta ValHi
   jmp Accum

TimeStart
   lda #0
   sta CIA2CRA
   lda #$ff
   sta CIA2TALo
   sta CIA2TAHi
   lda #%00010001         ; count PHI2, one run, load, start
   sta CIA2CRA
   rts

; Val = cycles since TimeStart, less the timing's own cost
TimeRead
   lda CIA2TALo
   eor #$ff
   sec
   sbc OverLo
   sta ValLo
   lda CIA2TAHi
   eor #$ff
   sbc OverHi
   sta ValHi
   bcs +
   lda #0
   sta ValLo
   sta ValHi
+  rts

WaitFrame
-  lda $d011
   bpl -
-  lda $d011
   bmi -
   rts

; X: slot. Adds Val to its sum and keeps its minimum and maximum
Accum
   clc
   lda Sum0,x
   adc ValLo
   sta Sum0,x
   lda Sum1,x
   adc ValHi
   sta Sum1,x
   bcc +
   inc Sum2,x
+  lda ValLo
   cmp MinLo,x
   lda ValHi
   sbc MinHi,x
   bcs +
   lda ValLo
   sta MinLo,x
   lda ValHi
   sta MinHi,x
+  lda ValLo
   cmp MaxLo,x
   lda ValHi
   sbc MaxHi,x
   bcc +
   lda ValLo
   sta MaxLo,x
   lda ValHi
   sta MaxHi,x
+  rts

ClearStats
   ldx #Slots-1
-  lda #0
   sta Sum0,x
   sta Sum1,x
   sta Sum2,x
   sta MaxLo,x
   sta MaxHi,x
   lda #$ff
   sta MinLo,x
   sta MinHi,x
   dex
   bpl -
   ldx #5
   lda #0
-  sta Errors,x
   dex
   bpl -
   sta PingErrors
   rts

Rewind
   lda #<TabA
   sta PtrA
   lda #>TabA
   sta PtrA+1
   lda #<TabB
   sta PtrB
   lda #>TabB
   sta PtrB+1
   rts

; Both tables: exponents $70-$8F, so no product or quotient overflows and every value fits
; AYINT; mantissa bytes zero one time in four, for MULSHF; signs at random
MakeTables
   lda #<TabA
   sta PtrA
   lda #>TabA
   sta PtrA+1
   lda #2
   sta Count
   ldx #0
--
   jsr Rand
   and #$1f
   clc
   adc #$70
   ldy #0
   sta (PtrA),y
-  iny
   jsr Rand
   and #3
   beq +
   jsr Rand
+  sta (PtrA),y
   cpy #4
   bne -
   clc
   lda PtrA
   adc #5
   sta PtrA
   bcc +
   inc PtrA+1
+  dex
   bne --
   dec Count
   bne --
   rts

Rand
   txa
   pha
   ldx #8
-  lsr Seed+1
   ror Seed
   bcc +
   lda Seed+1
   eor #$b4
   sta Seed+1
+  dex
   bne -
   pla
   tax
   lda Seed
   rts

; ---- the report ----

Report
   lda #<MsgTitle
   ldy #>MsgTitle
   jsr PrintMsg
   lda #<MsgPoll
   ldy #>MsgPoll
   ldx #0
   jsr PrintMinAvgMax
   lda #<MsgHalt
   ldy #>MsgHalt
   ldx #1
   jsr PrintMinAvgMax
   lda PingErrors
   beq +
   lda #<MsgPingErr
   ldy #>MsgPingErr
   jsr PrintMsg
   ldx PingErrors
   lda #0
   jsr LINPRT
+  lda #<MsgHeader
   ldy #>MsgHeader
   jsr PrintMsg
   lda #0
   sta Routine
-  jsr PrintRoutine
   inc Routine
   lda Routine
   cmp #6
   bne -
   lda #<MsgNs
   ldy #>MsgNs
   jsr PrintMsg
   lda #0
   sta Routine
-  jsr PrintRoutineNs
   inc Routine
   lda Routine
   cmp #6
   bne -
   lda #$0d
   jmp CHROUT

; A/Y: label; X: slot
PrintMinAvgMax
   stx Slot
   jsr PrintMsg
   ldx Slot
   lda MinHi,x
   pha
   lda MinLo,x
   tax
   pla
   jsr LINPRT
   jsr Space
   ldx Slot
   jsr PrintAvg
   jsr Space
   ldx Slot
   lda MaxHi,x
   pha
   lda MaxLo,x
   tax
   pla
   jmp LINPRT

; X: slot; prints its sum / 256
PrintAvg
   lda Sum2,x
   pha
   lda Sum1,x
   tax
   pla
   jmp LINPRT

PrintRoutine
   jsr PrintName
   lda Routine
   clc
   adc #2
   tax
   jsr PrintAvg
   jsr Space
   lda Routine
   clc
   adc #8
   tax
   jsr PrintAvg
   jsr Space
   ldx Routine
   lda Errors,x
   tax
   lda #0
   jmp LINPRT

PrintRoutineNs
   jsr PrintName
   lda Routine
   clc
   adc #14
   tax
   jsr PrintAvg
   jsr Space
   lda Routine
   clc
   adc #20
   tax
   jsr PrintAvg
   jsr Space
   lda Routine
   clc
   adc #26
   tax
   jmp PrintAvg

PrintName
   lda #$0d
   jsr CHROUT
   lda Routine
   asl
   asl
   asl
   tax
   ldy #8
-  lda Names,x
   jsr CHROUT
   inx
   dey
   bne -
   rts

Space
   lda #' '
   jmp CHROUT

PrintMsg
   sta PtrA
   sty PtrA+1
   ldy #0
-  lda (PtrA),y
   beq +
   jsr CHROUT
   iny
   bne -
+  rts

Entries  !word $b86a, $b853, $ba2b, $bb12, $bc9b, $b1bf
Names    !pet "faddt   fsubt   fmultt  fdivt   qint    ayint   "

MsgNoTR    !pet $0d, "no float hand-off test at $def0.", $0d
           !pet "launch this with the special io set to", $0d
           !pet "debug, on a teensyrom+.", $0d, 0
MsgRunning !pet $0d, "float hand-off test: about 10 s...", $0d, 0
MsgTitle   !pet $93, "float hand-off, 256 runs each", $0d, $0d
           !pet "c64 cycles     min avg max", 0
MsgPoll    !pet $0d, "ping, polled  ", 0
MsgHalt    !pet $0d, "ping, held    ", 0
MsgPingErr !pet $0d, "ping errors ", 0
MsgHeader  !pet $0d, $0d, "c64 cycles avg: rom, tr+, wrong", 0
MsgNs      !pet $0d, $0d, "tr ns avg: latency, compute, all", 0

Seed       !word $1d0f
Count      !byte 0
Routine    !byte 0
Slot       !byte 0
SaveD011   !byte 0
OverLo     !byte 0
OverHi     !byte 0
ValLo      !byte 0
ValHi      !byte 0
NatA       !byte 0
NatX       !byte 0
NatY       !byte 0
NatP       !byte 0
PingErrors !byte 0
Errors     !fill 6
Sum0       !fill Slots
Sum1       !fill Slots
Sum2       !fill Slots
MinLo      !fill Slots
MinHi      !fill Slots
MaxLo      !fill Slots
MaxHi      !fill Slots
Snap       !fill ZpLen
NatZp      !fill ZpLen
