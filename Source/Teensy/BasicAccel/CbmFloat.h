// SPDX-License-Identifier: MIT
//
// BASIC V2's float arithmetic in C, with the same results as the C64 ROM to the bit, including
// the zero-page side effects and the registers and flags each routine returns:
//
//   CbmFAddT  FADDT $B86A  FAC = ARG + FAC   `+`, and FADD ($B867) after CONUPK
//   CbmFSubT  FSUBT $B853  FAC = ARG - FAC   `-`, and FSUB ($B850) after CONUPK
//   CbmFMultT FMULTT $BA2B FAC = ARG * FAC   `*`, and FMULT ($BA28) after CONUPK
//   CbmFDivT  FDIVT $BB12  FAC = ARG / FAC   `/`, and FDIV ($BB0F) after CONUPK
//
// Each entry is where the ROM's routes into the routine meet. Every caller leaves N/Z set from
// $61 (FIN's digit loop, $BD8E, does it with LDX $61, so A is not always $61); the C reads $61
// instead. A cartridge sees only the zero page, not X, Y or the flags, so each call reports in
// CbmCpu.Set the registers and flags it determined. The rest keep whatever the caller had,
// exactly as the ROM leaves them.
//
// The code follows the ROM instruction by instruction, with the flags, except FMULTT's
// shift-and-add loop, which is done as five multiplies. Stack-page bytes the ROM writes below
// the stack pointer (return addresses, FDIVT's PHP) are not modeled.
//
// Not yet called by the firmware: Source/Teensy/tests/cbm-float.test.js checks it against the
// ROM, and tools/basic-accel/ uses it to model the speedup in VICE.
//
// Decimal mode is not modeled. With D set the ROM's ADCs are BCD and its results are garbage;
// BASIC never runs with D set.
#ifndef CBM_FLOAT_H
#define CBM_FLOAT_H

#include <stdbool.h>
#include <stdint.h>

#define CBM_FLAG_C 0x01
#define CBM_FLAG_Z 0x02
#define CBM_FLAG_V 0x40
#define CBM_FLAG_N 0x80

#define CBM_SET_A  0x0100
#define CBM_SET_X  0x0200
#define CBM_SET_Y  0x0400

// The zero page these routines read: $61-$70. They write $26-$2A (FDIVT's last quotient store,
// STA $29,X with X = 1, lands in $2A), $56 (FADDT's copy of FAC's rounding byte) and $61-$70.
// After a Native result Cpu->Zp holds scratch, none of it the ROM's: nothing is written back.
#define CBM_FLOAT_READ_FIRST  0x61
#define CBM_FLOAT_READ_LAST   0x70

typedef struct {
   uint8_t Zp[256];
   uint8_t A, X, Y, P;
   uint16_t Set;  // CBM_SET_* and CBM_FLAG_* this call determined
} CbmCpu;

typedef enum {
   CbmFloat_Done,    // Cpu holds the ROM's result; the ROM routine would RTS to its caller
   CbmFloat_Native,  // let the ROM run: it raises ?OVERFLOW or ?DIVISION BY ZERO itself
} CbmFloatResult;

// ---- one 6502 instruction each, on the registers, flags and zero page ----

static inline void CbmFlag(CbmCpu *Cpu, uint8_t Flag, bool On)
{
   Cpu->P = On ? Cpu->P | Flag : Cpu->P & ~Flag;
   Cpu->Set |= Flag;
}

static inline uint8_t CbmNZ(CbmCpu *Cpu, uint8_t Value)
{
   CbmFlag(Cpu, CBM_FLAG_Z, Value == 0);
   CbmFlag(Cpu, CBM_FLAG_N, Value & 0x80);
   return Value;
}

static inline bool CbmC(const CbmCpu *Cpu) { return Cpu->P & CBM_FLAG_C; }
static inline bool CbmN(const CbmCpu *Cpu) { return Cpu->P & CBM_FLAG_N; }
static inline bool CbmZ(const CbmCpu *Cpu) { return Cpu->P & CBM_FLAG_Z; }

static inline void CbmLda(CbmCpu *Cpu, uint8_t Value) { Cpu->A = CbmNZ(Cpu, Value); Cpu->Set |= CBM_SET_A; }
static inline void CbmLdx(CbmCpu *Cpu, uint8_t Value) { Cpu->X = CbmNZ(Cpu, Value); Cpu->Set |= CBM_SET_X; }
static inline void CbmLdy(CbmCpu *Cpu, uint8_t Value) { Cpu->Y = CbmNZ(Cpu, Value); Cpu->Set |= CBM_SET_Y; }

static inline uint8_t CbmAdc(CbmCpu *Cpu, uint8_t A, uint8_t B)
{
   unsigned Sum = A + B + CbmC(Cpu);
   uint8_t R = (uint8_t)Sum;
   CbmFlag(Cpu, CBM_FLAG_C, Sum > 0xff);
   CbmFlag(Cpu, CBM_FLAG_V, ~(A ^ B) & (A ^ R) & 0x80);
   return CbmNZ(Cpu, R);
}

static inline void CbmAdcA(CbmCpu *Cpu, uint8_t Value) { Cpu->A = CbmAdc(Cpu, Cpu->A, Value); Cpu->Set |= CBM_SET_A; }
static inline void CbmSbcA(CbmCpu *Cpu, uint8_t Value) { CbmAdcA(Cpu, (uint8_t)~Value); }
static inline void CbmEorA(CbmCpu *Cpu, uint8_t Value) { CbmLda(Cpu, Cpu->A ^ Value); }

static inline void CbmCmp(CbmCpu *Cpu, uint8_t Reg, uint8_t Value)
{
   CbmFlag(Cpu, CBM_FLAG_C, Reg >= Value);
   CbmNZ(Cpu, (uint8_t)(Reg - Value));
}

static inline uint8_t CbmAsl(CbmCpu *Cpu, uint8_t Value)
{
   CbmFlag(Cpu, CBM_FLAG_C, Value & 0x80);
   return CbmNZ(Cpu, (uint8_t)(Value << 1));
}

static inline uint8_t CbmLsr(CbmCpu *Cpu, uint8_t Value)
{
   CbmFlag(Cpu, CBM_FLAG_C, Value & 1);
   return CbmNZ(Cpu, Value >> 1);
}

static inline uint8_t CbmRol(CbmCpu *Cpu, uint8_t Value)
{
   bool In = CbmC(Cpu);
   CbmFlag(Cpu, CBM_FLAG_C, Value & 0x80);
   return CbmNZ(Cpu, (uint8_t)(Value << 1 | In));
}

static inline uint8_t CbmRor(CbmCpu *Cpu, uint8_t Value)
{
   bool In = CbmC(Cpu);
   CbmFlag(Cpu, CBM_FLAG_C, Value & 1);
   return CbmNZ(Cpu, (uint8_t)(Value >> 1 | In << 7));
}

static inline uint8_t CbmInc(CbmCpu *Cpu, uint8_t Value) { return CbmNZ(Cpu, Value + 1); }

// ---- the ROM's shared pieces ----

// $B8F7 (from $B8F9: $B8FB is STA $66 alone): FAC = 0
static inline CbmFloatResult CbmZeroFac(CbmCpu *Cpu)
{
   CbmLda(Cpu, 0);
   Cpu->Zp[0x61] = 0;
   Cpu->Zp[0x66] = 0;
   return CbmFloat_Done;
}

// $B936: a carry out of the mantissa shifts it right into a bumped exponent
static inline CbmFloatResult CbmCarryIntoFac(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   if (!CbmC(Cpu)) return CbmFloat_Done;
   Zp[0x61] = CbmInc(Cpu, Zp[0x61]);
   if (Zp[0x61] == 0) return CbmFloat_Native;  // $B97E ?OVERFLOW ERROR
   Zp[0x62] = CbmRor(Cpu, Zp[0x62]);
   Zp[0x63] = CbmRor(Cpu, Zp[0x63]);
   Zp[0x64] = CbmRor(Cpu, Zp[0x64]);
   Zp[0x65] = CbmRor(Cpu, Zp[0x65]);
   Zp[0x70] = CbmRor(Cpu, Zp[0x70]);
   return CbmFloat_Done;
}

// $B8D7 NORMLZ: whole bytes, then bits; FAC = 0 if it underflows
static CbmFloatResult CbmNormalize(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   CbmLdy(Cpu, 0);
   CbmLda(Cpu, Cpu->Y);
   CbmFlag(Cpu, CBM_FLAG_C, false);
   for (;;) {
      CbmLdx(Cpu, Zp[0x62]);
      if (!CbmZ(Cpu)) break;
      CbmLdx(Cpu, Zp[0x63]); Zp[0x62] = Cpu->X;
      CbmLdx(Cpu, Zp[0x64]); Zp[0x63] = Cpu->X;
      CbmLdx(Cpu, Zp[0x65]); Zp[0x64] = Cpu->X;
      CbmLdx(Cpu, Zp[0x70]); Zp[0x65] = Cpu->X;
      Zp[0x70] = Cpu->Y;
      CbmAdcA(Cpu, 8);
      CbmCmp(Cpu, Cpu->A, 0x20);
      if (CbmZ(Cpu)) return CbmZeroFac(Cpu);
   }
   while (!CbmN(Cpu)) {
      CbmAdcA(Cpu, 1);
      Zp[0x70] = CbmAsl(Cpu, Zp[0x70]);
      Zp[0x65] = CbmRol(Cpu, Zp[0x65]);
      Zp[0x64] = CbmRol(Cpu, Zp[0x64]);
      Zp[0x63] = CbmRol(Cpu, Zp[0x63]);
      Zp[0x62] = CbmRol(Cpu, Zp[0x62]);
   }
   CbmFlag(Cpu, CBM_FLAG_C, true);
   CbmSbcA(Cpu, Zp[0x61]);
   if (CbmC(Cpu)) return CbmZeroFac(Cpu);
   CbmEorA(Cpu, 0xff);
   CbmAdcA(Cpu, 1);
   Zp[0x61] = Cpu->A;
   return CbmCarryIntoFac(Cpu);
}

// $B947: negate FAC, sign and mantissa
static void CbmNegateFac(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   static const uint8_t Bytes[] = { 0x66, 0x62, 0x63, 0x64, 0x65, 0x70 };
   for (unsigned i = 0; i < sizeof Bytes; i++) {
      CbmLda(Cpu, Zp[Bytes[i]]);
      CbmEorA(Cpu, 0xff);
      Zp[Bytes[i]] = Cpu->A;
   }
   static const uint8_t Carry[] = { 0x70, 0x65, 0x64, 0x63, 0x62 };
   for (unsigned i = 0; i < sizeof Carry; i++)
      if ((Zp[Carry[i]] = CbmInc(Cpu, Zp[Carry[i]])) != 0) return;
}

// $B9A6, the bit loop of MULSHF: shift the mantissa at X (and A, its rounding byte) right once
// per INY until Y reaches 0. Entered at $B9B0 after the caller's own LSR of the top byte.
static void CbmShiftBits(CbmCpu *Cpu, uint8_t X, bool AtB9B0)
{
   uint8_t *Zp = Cpu->Zp;
   do {
      if (!AtB9B0) {
         Zp[X + 1] = CbmAsl(Cpu, Zp[X + 1]);
         if (CbmC(Cpu)) Zp[X + 1] = CbmInc(Cpu, Zp[X + 1]);
         Zp[X + 1] = CbmRor(Cpu, Zp[X + 1]);
         Zp[X + 1] = CbmRor(Cpu, Zp[X + 1]);
      }
      AtB9B0 = false;
      Zp[X + 2] = CbmRor(Cpu, Zp[X + 2]);
      Zp[X + 3] = CbmRor(Cpu, Zp[X + 3]);
      Zp[X + 4] = CbmRor(Cpu, Zp[X + 4]);
      Cpu->A = CbmRor(Cpu, Cpu->A);
      CbmLdy(Cpu, Cpu->Y + 1);
   } while (!CbmZ(Cpu));
   CbmFlag(Cpu, CBM_FLAG_C, false);
}

// $B999: MULSHF from its ADC, with A a negative shift count: whole bytes (filling with $68),
// then bits
static void CbmShift(CbmCpu *Cpu, uint8_t X)
{
   uint8_t *Zp = Cpu->Zp;
   for (;;) {
      CbmAdcA(Cpu, 8);
      if (!CbmN(Cpu) && !CbmZ(Cpu)) break;
      CbmLdy(Cpu, Zp[X + 4]); Zp[0x70] = Cpu->Y;
      CbmLdy(Cpu, Zp[X + 3]); Zp[X + 4] = Cpu->Y;
      CbmLdy(Cpu, Zp[X + 2]); Zp[X + 3] = Cpu->Y;
      CbmLdy(Cpu, Zp[X + 1]); Zp[X + 2] = Cpu->Y;
      CbmLdy(Cpu, Zp[0x68]); Zp[X + 1] = Cpu->Y;
   }
   CbmSbcA(Cpu, 8);
   CbmLdy(Cpu, Cpu->A);
   CbmLda(Cpu, Zp[0x70]);
   if (CbmC(Cpu)) CbmFlag(Cpu, CBM_FLAG_C, false);
   else CbmShiftBits(Cpu, X, false);
}

// $BAB7 MULDIV: add the exponents for a multiply or divide. Done means FAC is now 0 (the ROM
// drops MULDIV's caller and returns from it).
typedef enum { CbmMulDiv_Go, CbmMulDiv_Zero, CbmMulDiv_Overflow } CbmMulDivResult;

static CbmMulDivResult CbmMulDiv(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   CbmLda(Cpu, Zp[0x69]);
   if (CbmZ(Cpu)) { CbmZeroFac(Cpu); return CbmMulDiv_Zero; }
   CbmFlag(Cpu, CBM_FLAG_C, false);
   CbmAdcA(Cpu, Zp[0x61]);
   if (!CbmC(Cpu)) {
      if (!CbmN(Cpu)) { CbmZeroFac(Cpu); return CbmMulDiv_Zero; }
   }
   else {
      if (CbmN(Cpu)) return CbmMulDiv_Overflow;
      CbmFlag(Cpu, CBM_FLAG_C, false);  // then BIT $1410, whose flags the ADC replaces
   }
   CbmAdcA(Cpu, 0x80);
   Zp[0x61] = Cpu->A;
   if (CbmZ(Cpu)) Zp[0x66] = Cpu->A;  // exponent 0 goes on via $B8FB with the sign cleared
   else {
      CbmLda(Cpu, Zp[0x6f]);
      Zp[0x66] = Cpu->A;
   }
   return CbmMulDiv_Go;
}

// $BB8F: product or quotient from $26-$29 into FAC, then normalize
static CbmFloatResult CbmResultToFac(CbmCpu *Cpu)
{
   for (int i = 0; i < 4; i++) {
      CbmLda(Cpu, Cpu->Zp[0x26 + i]);
      Cpu->Zp[0x62 + i] = Cpu->A;
   }
   return CbmNormalize(Cpu);
}

// ---- the routines ----

static CbmFloatResult CbmFAddBody(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   if (Zp[0x61] == 0) {  // $BBFC MOVFA: FAC = ARG
      CbmLda(Cpu, Zp[0x6e]);
      Zp[0x66] = Cpu->A;
      for (uint8_t X = 5; X; X--) {
         CbmLda(Cpu, Zp[0x68 + X]);
         Zp[0x60 + X] = Cpu->A;
      }
      CbmLdx(Cpu, 0);
      Zp[0x70] = 0;
      return CbmFloat_Done;
   }
   CbmLdx(Cpu, Zp[0x70]);
   Zp[0x56] = Cpu->X;
   CbmLdx(Cpu, 0x69);
   CbmLda(Cpu, Zp[0x69]);
   CbmLdy(Cpu, Cpu->A);
   if (CbmZ(Cpu)) return CbmFloat_Done;  // ARG = 0
   CbmFlag(Cpu, CBM_FLAG_C, true);
   CbmSbcA(Cpu, Zp[0x61]);
   if (!CbmZ(Cpu)) {
      if (CbmC(Cpu)) {  // ARG's exponent is larger: shift FAC instead, and take ARG's exponent and sign
         Zp[0x61] = Cpu->Y;
         CbmLdy(Cpu, Zp[0x6e]);
         Zp[0x66] = Cpu->Y;
         CbmEorA(Cpu, 0xff);
         CbmAdcA(Cpu, 0);
         CbmLdy(Cpu, 0);
         Zp[0x56] = 0;
         CbmLdx(Cpu, 0x61);
      }
      else {
         CbmLdy(Cpu, 0);
         Zp[0x70] = 0;
      }
      CbmCmp(Cpu, Cpu->A, 0xf9);
      if (CbmN(Cpu)) CbmShift(Cpu, Cpu->X);
      else {
         CbmLdy(Cpu, Cpu->A);
         CbmLda(Cpu, Zp[0x70]);
         Zp[Cpu->X + 1] = CbmLsr(Cpu, Zp[Cpu->X + 1]);
         CbmShiftBits(Cpu, Cpu->X, true);
      }
   }

   // $B8A3: BIT $6F
   CbmFlag(Cpu, CBM_FLAG_N, Zp[0x6f] & 0x80);
   CbmFlag(Cpu, CBM_FLAG_V, Zp[0x6f] & 0x40);
   CbmFlag(Cpu, CBM_FLAG_Z, (Cpu->A & Zp[0x6f]) == 0);
   if (!CbmN(Cpu)) {  // same signs: add ($B8FE)
      CbmAdcA(Cpu, Zp[0x56]);
      Zp[0x70] = Cpu->A;
      for (int i = 3; i >= 0; i--) {
         CbmLda(Cpu, Zp[0x62 + i]);
         CbmAdcA(Cpu, Zp[0x6a + i]);
         Zp[0x62 + i] = Cpu->A;
      }
      return CbmCarryIntoFac(Cpu);
   }
   // opposite signs: subtract the shifted one from the other
   CbmLdy(Cpu, 0x61);
   CbmCmp(Cpu, Cpu->X, 0x69);
   if (!CbmZ(Cpu)) CbmLdy(Cpu, 0x69);
   CbmFlag(Cpu, CBM_FLAG_C, true);
   CbmEorA(Cpu, 0xff);
   CbmAdcA(Cpu, Zp[0x56]);
   Zp[0x70] = Cpu->A;
   for (int i = 4; i >= 1; i--) {
      CbmLda(Cpu, Zp[Cpu->Y + i]);
      CbmSbcA(Cpu, Zp[Cpu->X + i]);
      Zp[0x61 + i] = Cpu->A;
   }
   if (!CbmC(Cpu)) CbmNegateFac(Cpu);
   return CbmNormalize(Cpu);
}

static CbmFloatResult CbmFAddT(CbmCpu *Cpu)
{
   Cpu->Set = 0;
   return CbmFAddBody(Cpu);
}

static CbmFloatResult CbmFSubT(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   Cpu->Set = 0;
   CbmLda(Cpu, Zp[0x66]);
   CbmEorA(Cpu, 0xff);
   Zp[0x66] = Cpu->A;
   CbmEorA(Cpu, Zp[0x6e]);
   Zp[0x6f] = Cpu->A;
   CbmLda(Cpu, Zp[0x61]);
   return CbmFAddBody(Cpu);
}

static inline uint32_t CbmGet32(const uint8_t *Zp, uint8_t At)
{
   return (uint32_t)Zp[At] << 24 | (uint32_t)Zp[At + 1] << 16 | (uint32_t)Zp[At + 2] << 8 | Zp[At + 3];
}

static CbmFloatResult CbmFMultT(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   Cpu->Set = 0;
   if (Zp[0x61] == 0) return CbmFloat_Done;  // FAC = 0: the ROM returns at once

   CbmMulDivResult Exponent = CbmMulDiv(Cpu);
   if (Exponent == CbmMulDiv_Overflow) return CbmFloat_Native;
   if (Exponent == CbmMulDiv_Zero) return CbmFloat_Done;

   // The multiplier bytes, lowest first. A nonzero byte is eight shift-and-add steps of ARG
   // into the 40-bit window $26-$29:$70, which is one 32x8 multiply-add. A zero byte $70-$63
   // goes to MULSHF ($B983) instead, which shifts in $68 a byte at a time; entered with carry
   // clear, it also makes one extra arithmetic shift right of $26-$29 and drops the bit it
   // shifted out (the ROM's own result, kept here on purpose). The registers and flags this
   // leaves are all replaced by $BB8F and NORMLZ.
   const uint32_t Arg = CbmGet32(Zp, 0x6a);
   const uint8_t Multiplier[5] = { Zp[0x70], Zp[0x65], Zp[0x64], Zp[0x63], Zp[0x62] };
   uint32_t Product = 0;
   uint8_t Low = 0;
   bool Carry = CbmC(Cpu);
   for (int i = 0; i < 5; i++) {
      uint8_t M = Multiplier[i];
      if (M == 0 && i < 4) {
         Low = (uint8_t)Product;
         Product = Product >> 8 | (uint32_t)Zp[0x68] << 24;
         if (!Carry) Product = (uint32_t)((int32_t)Product >> 1);
         Carry = false;
      }
      else {
         uint64_t Sum = Product + (uint64_t)M * Arg;
         Product = (uint32_t)(Sum >> 8);
         Low = (uint8_t)Sum;
         Carry = true;
      }
   }
   Zp[0x26] = Product >> 24;
   Zp[0x27] = Product >> 16;
   Zp[0x28] = Product >> 8;
   Zp[0x29] = Product;
   Zp[0x70] = Low;
   return CbmResultToFac(Cpu);
}

// $BC1B: round FAC's mantissa by its rounding byte
static CbmFloatResult CbmRoundFac(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   CbmLda(Cpu, Zp[0x61]);
   if (CbmZ(Cpu)) return CbmFloat_Done;
   Zp[0x70] = CbmAsl(Cpu, Zp[0x70]);
   if (!CbmC(Cpu)) return CbmFloat_Done;
   static const uint8_t Carry[] = { 0x65, 0x64, 0x63, 0x62 };
   for (unsigned i = 0; i < sizeof Carry; i++)
      if ((Zp[Carry[i]] = CbmInc(Cpu, Zp[Carry[i]])) != 0) return CbmFloat_Done;
   return CbmCarryIntoFac(Cpu);  // $B938, carry still set from the ASL
}

static CbmFloatResult CbmFDivT(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   Cpu->Set = 0;
   if (Zp[0x61] == 0) return CbmFloat_Native;  // $BB8A ?DIVISION BY ZERO ERROR
   if (CbmRoundFac(Cpu) == CbmFloat_Native) return CbmFloat_Native;
   CbmLda(Cpu, 0);
   CbmFlag(Cpu, CBM_FLAG_C, true);
   CbmSbcA(Cpu, Zp[0x61]);
   Zp[0x61] = Cpu->A;
   CbmMulDivResult Exponent = CbmMulDiv(Cpu);
   if (Exponent == CbmMulDiv_Overflow) return CbmFloat_Native;
   if (Exponent == CbmMulDiv_Zero) return CbmFloat_Done;
   Zp[0x61] = CbmInc(Cpu, Zp[0x61]);
   if (CbmZ(Cpu)) return CbmFloat_Native;

   // Long division of ARG's mantissa by FAC's, a quotient bit per step, into $26-$29 and then
   // two bits of rounding byte. A holds the byte being built, with a 1 that falls out when
   // it is full. Each step's compare result goes through the stack (PHP/PLP).
   CbmLdx(Cpu, 0xfc);
   CbmLda(Cpu, 1);
   uint8_t SavedP = 0;
   uint16_t SavedSet = 0;
   enum { Compare, Push, Shift } Step = Compare;
   for (;;) {
      if (Step == Compare) {  // $BB29
         static const uint8_t Pairs[][2] = { { 0x6a, 0x62 }, { 0x6b, 0x63 }, { 0x6c, 0x64 }, { 0x6d, 0x65 } };
         for (unsigned i = 0; i < 4; i++) {
            CbmLdy(Cpu, Zp[Pairs[i][0]]);
            CbmCmp(Cpu, Cpu->Y, Zp[Pairs[i][1]]);
            if (!CbmZ(Cpu)) break;
         }
         Step = Push;
      }
      if (Step == Push) {  // $BB3F
         SavedP = Cpu->P;
         SavedSet = Cpu->Set & (CBM_FLAG_C | CBM_FLAG_Z | CBM_FLAG_V | CBM_FLAG_N);
         Cpu->A = CbmRol(Cpu, Cpu->A);
         if (CbmC(Cpu)) {
            CbmLdx(Cpu, Cpu->X + 1);
            Zp[(uint8_t)(0x29 + Cpu->X)] = Cpu->A;
            if (CbmZ(Cpu)) CbmLda(Cpu, 0x40);
            else if (!CbmN(Cpu)) {  // $BB7E: the last two bits are the rounding byte
               for (int i = 0; i < 6; i++) Cpu->A = CbmAsl(Cpu, Cpu->A);
               Zp[0x70] = Cpu->A;
               Cpu->P = SavedP;
               Cpu->Set = (Cpu->Set & ~(CBM_FLAG_C | CBM_FLAG_Z | CBM_FLAG_V | CBM_FLAG_N)) | SavedSet;
               return CbmResultToFac(Cpu);
            }
            else CbmLda(Cpu, 1);
         }
         // $BB4C
         Cpu->P = SavedP;
         Cpu->Set = (Cpu->Set & ~(CBM_FLAG_C | CBM_FLAG_Z | CBM_FLAG_V | CBM_FLAG_N)) | SavedSet;
         if (CbmC(Cpu)) {  // $BB5D: ARG -= FAC
            CbmLdy(Cpu, Cpu->A);
            for (int i = 3; i >= 0; i--) {
               CbmLda(Cpu, Zp[0x6a + i]);
               CbmSbcA(Cpu, Zp[0x62 + i]);
               Zp[0x6a + i] = Cpu->A;
            }
            CbmLda(Cpu, Cpu->Y);
         }
         Step = Shift;
      }
      // $BB4F: ARG <<= 1
      Zp[0x6d] = CbmAsl(Cpu, Zp[0x6d]);
      Zp[0x6c] = CbmRol(Cpu, Zp[0x6c]);
      Zp[0x6b] = CbmRol(Cpu, Zp[0x6b]);
      Zp[0x6a] = CbmRol(Cpu, Zp[0x6a]);
      if (CbmC(Cpu)) Step = Push;
      else if (CbmN(Cpu)) Step = Compare;
      else Step = Push;
   }
}

#endif
