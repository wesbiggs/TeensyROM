// SPDX-License-Identifier: MIT
//
// BASIC V2's FMULTT ($BA2B: FAC *= ARG) in C, with the same result as the C64 ROM to the bit,
// including the zero-page side effects and the registers and flags it returns.
//
// $BA2B is where both callers meet: FMULT ($BA28) arrives after CONUPK has loaded ARG from
// memory, and the expression evaluator's `*` arrives by RTS from $AE80. Both leave A = $61
// with N/Z set from it. Nothing else about the entry state is known to a cartridge, so a
// result says which registers and flags it determines (CbmFMult.Set); the rest keep whatever
// the caller had, exactly as the ROM leaves them.
//
// Not yet called by the firmware: Source/Teensy/tests/cbm-fmult.test.js checks it against the
// ROM, and tools/basic-accel/ uses it to model the speedup in VICE.
//
// Decimal mode is not modeled. With D set the ROM's ADCs are BCD and its product is garbage;
// BASIC never runs with D set.
#ifndef CBM_FMULT_H
#define CBM_FMULT_H

#include <stdbool.h>
#include <stdint.h>

#define CBM_FLAG_C 0x01
#define CBM_FLAG_Z 0x02
#define CBM_FLAG_V 0x40
#define CBM_FLAG_N 0x80

#define CBM_SET_A  0x0100
#define CBM_SET_X  0x0200
#define CBM_SET_Y  0x0400

// Zero-page bytes FMULTT reads and may write. Everything else in the zero page is untouched.
//   reads:  $61-$65 FAC exponent and mantissa, $68 (shifted in by MULSHF), $69-$6D ARG,
//           $6F sign of the product, $70 FAC rounding byte
//   writes: $26-$29 product, $61-$66 FAC, $70
#define CBM_FMULT_READ_FIRST  0x61
#define CBM_FMULT_READ_LAST   0x70

typedef struct {
   uint8_t Zp[256];
   uint8_t A, X, Y, P;
   uint16_t Set;  // CBM_SET_* and CBM_FLAG_* this call determined
} CbmCpu;

typedef enum {
   CbmFMult_Done,    // Cpu holds the ROM's result; the ROM routine would RTS to its caller
   CbmFMult_Native,  // let the ROM run: FAC is zero (it returns at once) or the product overflows
} CbmFMultResult;

static inline uint8_t CbmAdc(CbmCpu *Cpu, uint8_t A, uint8_t B)
{
   unsigned Sum = A + B + (Cpu->P & CBM_FLAG_C);
   uint8_t R = (uint8_t)Sum;
   Cpu->P &= ~(CBM_FLAG_C | CBM_FLAG_Z | CBM_FLAG_V | CBM_FLAG_N);
   if (Sum > 0xff) Cpu->P |= CBM_FLAG_C;
   if (~(A ^ B) & (A ^ R) & 0x80) Cpu->P |= CBM_FLAG_V;
   if (R == 0) Cpu->P |= CBM_FLAG_Z;
   Cpu->P |= R & CBM_FLAG_N;
   return R;
}

static inline void CbmLoad(CbmCpu *Cpu, uint8_t *Reg, uint8_t Value)
{
   *Reg = Value;
   Cpu->P &= ~(CBM_FLAG_Z | CBM_FLAG_N);
   if (Value == 0) Cpu->P |= CBM_FLAG_Z;
   Cpu->P |= Value & CBM_FLAG_N;
}

// $B8F7: FAC = 0. Reached by RTS straight to FMULTT's caller.
static inline CbmFMultResult CbmFacZero(CbmCpu *Cpu)
{
   CbmLoad(Cpu, &Cpu->A, 0);
   Cpu->Zp[0x61] = 0;
   Cpu->Zp[0x66] = 0;
   Cpu->Set |= CBM_SET_A | CBM_FLAG_N | CBM_FLAG_Z;
   return CbmFMult_Done;
}

static inline uint32_t CbmGet32(const uint8_t *Zp, uint8_t At)
{
   return (uint32_t)Zp[At] << 24 | (uint32_t)Zp[At + 1] << 16 | (uint32_t)Zp[At + 2] << 8 | Zp[At + 3];
}

static inline void CbmPut32(uint8_t *Zp, uint8_t At, uint32_t Value)
{
   Zp[At] = Value >> 24;
   Zp[At + 1] = Value >> 16;
   Zp[At + 2] = Value >> 8;
   Zp[At + 3] = Value;
}

static CbmFMultResult CbmFMultT(CbmCpu *Cpu)
{
   uint8_t *Zp = Cpu->Zp;
   Cpu->Set = 0;

   if (Zp[0x61] == 0) return CbmFMult_Native;

   // MULDIV ($BAB7): add the exponents
   if (Zp[0x69] == 0) return CbmFacZero(Cpu);
   Cpu->P &= ~CBM_FLAG_C;
   uint8_t A = CbmAdc(Cpu, Zp[0x69], Zp[0x61]);
   Cpu->Set |= CBM_FLAG_C | CBM_FLAG_V;
   if (Cpu->P & CBM_FLAG_C) {
      if (A & 0x80) return CbmFMult_Native;  // $BADF: ?OVERFLOW ERROR
      Cpu->P &= ~CBM_FLAG_C;
   }
   else if (!(A & 0x80)) return CbmFacZero(Cpu);  // $BAC4 BPL: underflow
   A = CbmAdc(Cpu, A, 0x80);
   Zp[0x61] = A;
   Zp[0x66] = A ? Zp[0x6F] : 0;  // exponent 0 goes on via $B8FB with the sign cleared

   // The multiplier bytes, lowest first. A nonzero byte is eight shift-and-add steps of ARG
   // into the 40-bit window $26-$29:$70, which is one 32x8 multiply-add. A zero byte $70-$63
   // goes to MULSHF ($B983) instead, which shifts in $68 a byte at a time; entered with carry
   // clear, it also makes one extra arithmetic shift right of $26-$29 and drops the bit it
   // shifted out (the ROM's own result, kept here on purpose).
   const uint32_t Arg = CbmGet32(Zp, 0x6A);
   const uint8_t Multiplier[5] = { Zp[0x70], Zp[0x65], Zp[0x64], Zp[0x63], Zp[0x62] };
   uint32_t Product = 0;
   uint8_t Low = 0;
   bool Carry = Cpu->P & CBM_FLAG_C;
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
   CbmPut32(Zp, 0x26, Product);
   CbmPut32(Zp, 0x62, Product);  // $BB8F
   Zp[0x70] = Low;

   // NORMLZ ($B8D7): whole bytes, then bits
   Cpu->P &= ~CBM_FLAG_C;
   Cpu->Y = 0;
   Cpu->Set |= CBM_SET_A | CBM_SET_X | CBM_SET_Y | CBM_FLAG_N | CBM_FLAG_Z;
   A = 0;
   while ((Cpu->X = Zp[0x62]) == 0) {
      Zp[0x62] = Zp[0x63];
      Zp[0x63] = Zp[0x64];
      Zp[0x64] = Zp[0x65];
      Cpu->X = Zp[0x65] = Zp[0x70];
      Zp[0x70] = 0;
      A = CbmAdc(Cpu, A, 8);
      if (A == 0x20) {
         Cpu->P |= CBM_FLAG_C;  // CMP #$20, equal
         return CbmFacZero(Cpu);
      }
   }
   while (!(Zp[0x62] & 0x80)) {
      A = CbmAdc(Cpu, A, 1);
      uint64_t Mantissa = (uint64_t)CbmGet32(Zp, 0x62) << 8 | Zp[0x70];
      Mantissa <<= 1;
      CbmPut32(Zp, 0x62, (uint32_t)(Mantissa >> 8));
      Zp[0x70] = (uint8_t)Mantissa;
   }
   Cpu->P |= CBM_FLAG_C;
   A = CbmAdc(Cpu, A, (uint8_t)~Zp[0x61]);  // SEC / SBC $61
   if (Cpu->P & CBM_FLAG_C) return CbmFacZero(Cpu);
   A = CbmAdc(Cpu, (uint8_t)~A, 1);  // carry is clear here, and stays clear: A was not $61
   Zp[0x61] = A;
   Cpu->A = A;
   return CbmFMult_Done;
}

#endif
