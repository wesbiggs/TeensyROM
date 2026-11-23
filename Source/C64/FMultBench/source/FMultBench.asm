; FMultBench: times a long series of BASIC ROM float multiplies (FMULT, $BA28).
;
; Each call loads FAC from one table of 256 floats (MOVFM) and multiplies it by the matching
; float of a second table, then folds the FAC bytes and the registers FMULT returned into a
; Fletcher-16 sum. The loop runs twice, first calling an RTS instead of FMULT, so the difference
; between the two passes is FMULT's own time. CIA2's timers A and B, chained, count the cycles.
;
; The sum is the check: a run that replaces FMULT has to print the same sum as the ROM.
; tools/basic-accel/vice-fmult.mjs runs it in VICE and reads Results.

   !cpu 6510

MOVFM   = $bba2
FMULT   = $ba28
CHROUT  = $ffd2

CIA2TALo = $dd04
CIA2TAHi = $dd05
CIA2TBLo = $dd06
CIA2TBHi = $dd07
CIA2CRA  = $dd0e
CIA2CRB  = $dd0f

Floats  = 256
TabA    = $3000
TabB    = TabA + Floats * 5

Count   = 4500       ; calls per pass; about 12 s of FMULTs on a PAL C64

Ptr     = $fb        ; free zero page, also used by MakeTables
PtrB    = $fd

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
   jsr MakeTables

   lda #<Empty
   ldy #>Empty
   jsr RunPass
   ldx #3
-  lda Cycles,x
   sta Results,x
   dex
   bpl -

   lda #<FMULT
   ldy #>FMULT
   jsr RunPass
   ldx #3
-  lda Cycles,x
   sta Results+4,x
   dex
   bpl -
   lda Sum1
   sta Results+8
   lda Sum2
   sta Results+9
   lda #1
   sta Results+10

   ldx #0
-  lda Title,x
   beq +
   jsr CHROUT
   inx
   bne -
+  ldx #0
   jsr PrintLong
   lda #<MsgFMult
   ldy #>MsgFMult
   jsr PrintMsg
   ldx #4
   jsr PrintLong
   lda #<MsgSum
   ldy #>MsgSum
   jsr PrintMsg
   lda Sum2
   jsr PrintHex
   lda Sum1
   jsr PrintHex
   lda #$0d
   jmp CHROUT

Empty
   rts

; A/Y: the routine to call. Leaves the cycle count in Cycles, the sum in Sum1/Sum2.
RunPass
   sta Call+1
   sty Call+2
   lda #0
   sta Sum1
   sta Sum2
   lda #<Count
   sta Left
   lda #>Count
   sta Left+1
   jsr Rewind

   lda #0
   sta CIA2CRA
   sta CIA2CRB
   lda #$ff
   sta CIA2TALo
   sta CIA2TAHi
   sta CIA2TBLo
   sta CIA2TBHi
   lda #%01010001     ; B: count A's underflows, load, start
   sta CIA2CRB
   lda #%00010001     ; A: count PHI2, continuous, load, start
   sta CIA2CRA

Loop
   lda Ptr
   ldy Ptr+1
   jsr MOVFM
   lda PtrB
   ldy PtrB+1
Call
   jsr FMULT
   php
   sta SaveA
   stx SaveX
   sty SaveY
   pla
   jsr Fold
   lda SaveA
   jsr Fold
   lda SaveX
   jsr Fold
   lda SaveY
   jsr Fold
   ldx #$61
-  lda $00,x
   jsr Fold
   inx
   cpx #$67
   bne -
   lda $70
   jsr Fold

   clc
   lda Ptr
   adc #5
   sta Ptr
   bcc +
   inc Ptr+1
+  clc
   lda PtrB
   adc #5
   sta PtrB
   bcc +
   inc PtrB+1
+  lda Ptr+1
   cmp #>TabB
   bne +
   jsr Rewind
+  lda Left
   bne +
   dec Left+1
+  dec Left
   lda Left
   ora Left+1
   bne Loop

   lda #0
   sta CIA2CRA
   lda CIA2TALo
   eor #$ff
   sta Cycles
   lda CIA2TAHi
   eor #$ff
   sta Cycles+1
   lda CIA2TBLo
   eor #$ff
   sta Cycles+2
   lda CIA2TBHi
   eor #$ff
   sta Cycles+3
   rts

Rewind
   lda #<TabA
   sta Ptr
   lda #>TabA
   sta Ptr+1
   lda #<TabB
   sta PtrB
   lda #>TabB
   sta PtrB+1
   rts

Fold
   clc
   adc Sum1
   sta Sum1
   clc
   adc Sum2
   sta Sum2
   rts

; Fills both tables with floats whose exponents are $70-$8F, so no product overflows or
; underflows, and whose mantissa bytes are zero one time in four: a zero multiplier byte takes
; FMULT through MULSHF.
MakeTables
   lda #<TabA
   sta Ptr
   lda #>TabA
   sta Ptr+1
   ldx #0             ; 512 floats: two rounds of 256
   lda #2
   sta Left
--
   jsr Rand
   and #$1f
   clc
   adc #$70
   ldy #0
   sta (Ptr),y
-  iny
   jsr Rand
   and #3
   beq +
   jsr Rand
+  sta (Ptr),y
   cpy #4
   bne -
   clc
   lda Ptr
   adc #5
   sta Ptr
   bcc +
   inc Ptr+1
+  dex
   bne --
   dec Left
   bne --
   rts

; 16-bit Galois LFSR, eight steps per byte
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

; X: offset into Results of a 32-bit count; prints it as 8 hex digits
PrintLong
   txa
   pha
   lda Results+3,x
   jsr PrintHex
   pla
   pha
   tax
   lda Results+2,x
   jsr PrintHex
   pla
   pha
   tax
   lda Results+1,x
   jsr PrintHex
   pla
   tax
   lda Results,x
   jmp PrintHex

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
   sta Ptr
   sty Ptr+1
   ldy #0
-  lda (Ptr),y
   beq +
   jsr CHROUT
   iny
   bne -
+  rts

Title    !pet $0d, "fmult bench", $0d, "empty cycles $", 0
MsgFMult !pet $0d, "fmult cycles $", 0
MsgSum   !pet $0d, "sum $", 0

Seed     !word $1d0f
Left     !word 0
SaveA    !byte 0
SaveX    !byte 0
SaveY    !byte 0
Sum1     !byte 0
Sum2     !byte 0
Cycles   !byte 0, 0, 0, 0
; empty-pass cycles (4), FMULT-pass cycles (4), sum (2), done flag (1)
Results  !byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
