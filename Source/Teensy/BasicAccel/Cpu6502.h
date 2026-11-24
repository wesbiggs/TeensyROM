// SPDX-License-Identifier: MIT
//
// An NMOS 6502 for running C64 code on the Teensy, without the C64's timing: each instruction
// runs whole, and the cycles it would take on the real part are counted, not waited for.
//
// The file that includes this one defines the bus, before or after the include:
//
//   static inline uint8_t Cpu6502Read(uint16_t Addr);
//   static inline void Cpu6502Write(uint16_t Addr, uint8_t Value);
//   static inline void Cpu6502Modify(uint16_t Addr, uint8_t Old, uint8_t New);
//   static inline bool Cpu6502IrqLine(void);   // true while something holds IRQ low
//   static inline bool Cpu6502TakeNmi(void);   // true once per NMI edge
//
// Both are asked between instructions, live: an IRQ handler that acknowledges its source drops
// the line before its RTI.
//
// Cpu6502Modify is the write of a read-modify-write instruction (ASL, LSR, ROL, ROR, INC, DEC on
// memory). The NMOS part writes Old back first and New on the next cycle; on I/O that is
// visible (`INC $D019` acknowledges a VIC interrupt by it), on RAM it is not.
//
// Every operand is read once: the real part's dummy reads (an indexed access that crosses a page
// reads the unfixed address first) do not happen. The 151 documented opcodes run; the twelve
// JAM opcodes stop the run with PC on them, which is how a caller patches in its own routines;
// any other undocumented opcode stops it the same way, as Cpu6502_Undefined.
//
// Decimal mode follows the NMOS flags, as tools/lib/mos6502.mjs does, which the test checks
// this against.
#ifndef CPU6502_H
#define CPU6502_H

#include <stdbool.h>
#include <stdint.h>

// Where the code goes. The firmware defines it as FLASHMEM: none of this runs on the PHI2 path.
#ifndef CPU6502_FN
#define CPU6502_FN
#endif

#define CPU6502_C 0x01
#define CPU6502_Z 0x02
#define CPU6502_I 0x04
#define CPU6502_D 0x08
#define CPU6502_B 0x10
#define CPU6502_U 0x20
#define CPU6502_V 0x40
#define CPU6502_N 0x80

typedef struct {
   uint16_t PC;
   uint8_t A, X, Y, S;
   uint8_t P;              // B is never held here, only pushed; U always reads set
   uint64_t Cycles;        // what the instructions run so far take on a real 6502
   uint64_t Instructions;
} Cpu6502;

typedef enum {
   Cpu6502_Budget,     // Cycles reached the limit
   Cpu6502_Jam,        // PC is on a JAM opcode ($02, $12, ... $F2)
   Cpu6502_Undefined,  // PC is on another undocumented opcode
} Cpu6502Stop;

static inline uint8_t Cpu6502Read(uint16_t Addr);
static inline void Cpu6502Write(uint16_t Addr, uint8_t Value);
static inline void Cpu6502Modify(uint16_t Addr, uint8_t Old, uint8_t New);
static inline bool Cpu6502IrqLine(void);
static inline bool Cpu6502TakeNmi(void);

#define CPU6502_VECTOR_NMI   0xfffa
#define CPU6502_VECTOR_RESET 0xfffc
#define CPU6502_VECTOR_IRQ   0xfffe

static inline uint16_t Cpu6502Read16(uint16_t Addr)
{
   return Cpu6502Read(Addr) | Cpu6502Read((uint16_t)(Addr + 1)) << 8;
}

static inline void Cpu6502Reset(Cpu6502 *Cpu)
{
   Cpu->S = 0xfd;
   Cpu->P = CPU6502_U | CPU6502_I;
   Cpu->PC = Cpu6502Read16(CPU6502_VECTOR_RESET);
}

static inline void Cpu6502Push(Cpu6502 *R, uint8_t Value)
{
   Cpu6502Write(0x100 | R->S, Value);
   R->S--;
}

static inline uint8_t Cpu6502Pull(Cpu6502 *R)
{
   R->S++;
   return Cpu6502Read(0x100 | R->S);
}

static inline void Cpu6502Interrupt(Cpu6502 *R, uint16_t Vector, uint8_t Break)
{
   Cpu6502Push(R, R->PC >> 8);
   Cpu6502Push(R, (uint8_t)R->PC);
   Cpu6502Push(R, (R->P & ~CPU6502_B) | CPU6502_U | Break);
   R->P |= CPU6502_I;
   R->PC = Cpu6502Read16(Vector);
   R->Cycles += 7;
}

// RTS, for a caller that answered a routine itself: return to whoever called it
static inline void Cpu6502Rts(Cpu6502 *R)
{
   uint16_t Lo = Cpu6502Pull(R);
   R->PC = (uint16_t)((Lo | Cpu6502Pull(R) << 8) + 1);
   R->Cycles += 6;
}

static inline uint8_t Cpu6502NZ(Cpu6502 *R, uint8_t V)
{
   R->P = (R->P & ~(CPU6502_N | CPU6502_Z)) | (V & CPU6502_N) | (V ? 0 : CPU6502_Z);
   return V;
}

static inline void Cpu6502Adc(Cpu6502 *R, uint8_t V);
static inline void Cpu6502Sbc(Cpu6502 *R, uint8_t V);

// One instruction, with PC already past the opcode. Inlined into its two callers so that R, a
// local copy, stays in registers: the bus writes through uint8_t, which may alias anything.
static inline __attribute__((always_inline)) Cpu6502Stop Cpu6502Op(Cpu6502 *R, uint8_t Op)
{
   uint16_t Ea = 0;
   uint8_t V, T;
   unsigned Sum;

#define FETCH()    Cpu6502Read(R->PC++)
#define CY(n)      (R->Cycles += (n))
#define SETF(f, on) (R->P = (on) ? (R->P | (f)) : (R->P & ~(f)))
#define NZ(v)      Cpu6502NZ(R, (uint8_t)(v))
#define CROSS(base, ea) ((((base) ^ (ea)) & 0xff00) != 0)
   // addressing: each sets Ea; the read forms add the page-cross cycle
#define A_ZP()     (Ea = FETCH())
#define A_ZPX()    (Ea = (uint8_t)(FETCH() + R->X))
#define A_ZPY()    (Ea = (uint8_t)(FETCH() + R->Y))
#define A_ABS()    (Ea = FETCH(), Ea |= FETCH() << 8)
#define A_ABSI(i, rd) do { uint16_t B_ = FETCH(); B_ |= FETCH() << 8; Ea = (uint16_t)(B_ + (i)); if ((rd) && CROSS(B_, Ea)) CY(1); } while (0)
#define A_INDX()   do { uint8_t Z_ = (uint8_t)(FETCH() + R->X); Ea = Cpu6502Read(Z_) | Cpu6502Read((uint8_t)(Z_ + 1)) << 8; } while (0)
#define A_INDY(rd) do { uint8_t Z_ = FETCH(); uint16_t B_ = Cpu6502Read(Z_) | Cpu6502Read((uint8_t)(Z_ + 1)) << 8; Ea = (uint16_t)(B_ + R->Y); if ((rd) && CROSS(B_, Ea)) CY(1); } while (0)
   // the eight ways each ALU group (ORA, AND, EOR, ADC, LDA, CMP, SBC) reaches its operand
#define GROUP1(base, OPER) \
   case base + 0x09: CY(2); V = FETCH(); OPER; break;                          \
   case base + 0x05: CY(3); A_ZP(); V = Cpu6502Read(Ea); OPER; break;          \
   case base + 0x15: CY(4); A_ZPX(); V = Cpu6502Read(Ea); OPER; break;         \
   case base + 0x0d: CY(4); A_ABS(); V = Cpu6502Read(Ea); OPER; break;         \
   case base + 0x1d: CY(4); A_ABSI(R->X, 1); V = Cpu6502Read(Ea); OPER; break; \
   case base + 0x19: CY(4); A_ABSI(R->Y, 1); V = Cpu6502Read(Ea); OPER; break; \
   case base + 0x01: CY(6); A_INDX(); V = Cpu6502Read(Ea); OPER; break;        \
   case base + 0x11: CY(5); A_INDY(1); V = Cpu6502Read(Ea); OPER; break;
   // ASL, ROL, LSR, ROR, DEC, INC on memory; base is the zero-page opcode
#define RMW(base, OPER) \
   case base + 0x00: CY(5); A_ZP(); goto Rmw_##base;                \
   case base + 0x10: CY(6); A_ZPX(); goto Rmw_##base;               \
   case base + 0x08: CY(6); A_ABS(); goto Rmw_##base;               \
   case base + 0x18: CY(7); A_ABSI(R->X, 0); goto Rmw_##base;       \
   Rmw_##base: T = Cpu6502Read(Ea); V = T; OPER; Cpu6502Modify(Ea, T, V); break;
#define BRANCH(op, cond) \
   case op: CY(2); V = FETCH(); if (cond) { Ea = (uint16_t)(R->PC + (int8_t)V); CY(CROSS(R->PC, Ea) ? 2 : 1); R->PC = Ea; } break;

#define O_ORA  (R->A = NZ(R->A | V))
#define O_AND  (R->A = NZ(R->A & V))
#define O_EOR  (R->A = NZ(R->A ^ V))
#define O_LDA  (R->A = NZ(V))
#define O_CMP  (SETF(CPU6502_C, R->A >= V), (void)NZ(R->A - V))
#define O_ADC  Cpu6502Adc(R, V)
#define O_SBC  Cpu6502Sbc(R, V)
#define O_ASL  (SETF(CPU6502_C, V & 0x80), V = NZ(V << 1))
#define O_LSR  (SETF(CPU6502_C, V & 0x01), V = NZ(V >> 1))
#define O_ROL  (Sum = R->P & CPU6502_C, SETF(CPU6502_C, V & 0x80), V = NZ(V << 1 | Sum))
#define O_ROR  (Sum = R->P & CPU6502_C, SETF(CPU6502_C, V & 0x01), V = NZ(V >> 1 | Sum << 7))
#define O_DEC  (V = NZ(V - 1))
#define O_INC  (V = NZ(V + 1))

   switch (Op)
   {
      GROUP1(0x00, O_ORA)
      GROUP1(0x20, O_AND)
      GROUP1(0x40, O_EOR)
      GROUP1(0x60, O_ADC)
      GROUP1(0xa0, O_LDA)
      GROUP1(0xc0, O_CMP)
      GROUP1(0xe0, O_SBC)

      RMW(0x06, O_ASL)
      RMW(0x26, O_ROL)
      RMW(0x46, O_LSR)
      RMW(0x66, O_ROR)
      RMW(0xc6, O_DEC)
      RMW(0xe6, O_INC)
      case 0x0a: CY(2); V = R->A; O_ASL; R->A = V; break;
      case 0x2a: CY(2); V = R->A; O_ROL; R->A = V; break;
      case 0x4a: CY(2); V = R->A; O_LSR; R->A = V; break;
      case 0x6a: CY(2); V = R->A; O_ROR; R->A = V; break;

      // STA has no immediate form; $89 is undocumented
      case 0x85: CY(3); A_ZP(); Cpu6502Write(Ea, R->A); break;
      case 0x95: CY(4); A_ZPX(); Cpu6502Write(Ea, R->A); break;
      case 0x8d: CY(4); A_ABS(); Cpu6502Write(Ea, R->A); break;
      case 0x9d: CY(5); A_ABSI(R->X, 0); Cpu6502Write(Ea, R->A); break;
      case 0x99: CY(5); A_ABSI(R->Y, 0); Cpu6502Write(Ea, R->A); break;
      case 0x81: CY(6); A_INDX(); Cpu6502Write(Ea, R->A); break;
      case 0x91: CY(6); A_INDY(0); Cpu6502Write(Ea, R->A); break;
      case 0x86: CY(3); A_ZP(); Cpu6502Write(Ea, R->X); break;
      case 0x96: CY(4); A_ZPY(); Cpu6502Write(Ea, R->X); break;
      case 0x8e: CY(4); A_ABS(); Cpu6502Write(Ea, R->X); break;
      case 0x84: CY(3); A_ZP(); Cpu6502Write(Ea, R->Y); break;
      case 0x94: CY(4); A_ZPX(); Cpu6502Write(Ea, R->Y); break;
      case 0x8c: CY(4); A_ABS(); Cpu6502Write(Ea, R->Y); break;

      case 0xa2: CY(2); R->X = NZ(FETCH()); break;
      case 0xa6: CY(3); A_ZP(); R->X = NZ(Cpu6502Read(Ea)); break;
      case 0xb6: CY(4); A_ZPY(); R->X = NZ(Cpu6502Read(Ea)); break;
      case 0xae: CY(4); A_ABS(); R->X = NZ(Cpu6502Read(Ea)); break;
      case 0xbe: CY(4); A_ABSI(R->Y, 1); R->X = NZ(Cpu6502Read(Ea)); break;
      case 0xa0: CY(2); R->Y = NZ(FETCH()); break;
      case 0xa4: CY(3); A_ZP(); R->Y = NZ(Cpu6502Read(Ea)); break;
      case 0xb4: CY(4); A_ZPX(); R->Y = NZ(Cpu6502Read(Ea)); break;
      case 0xac: CY(4); A_ABS(); R->Y = NZ(Cpu6502Read(Ea)); break;
      case 0xbc: CY(4); A_ABSI(R->X, 1); R->Y = NZ(Cpu6502Read(Ea)); break;

      case 0xe0: CY(2); V = FETCH(); goto Cpx;
      case 0xe4: CY(3); A_ZP(); V = Cpu6502Read(Ea); goto Cpx;
      case 0xec: CY(4); A_ABS(); V = Cpu6502Read(Ea);
      Cpx: SETF(CPU6502_C, R->X >= V); (void)NZ(R->X - V); break;
      case 0xc0: CY(2); V = FETCH(); goto Cpy;
      case 0xc4: CY(3); A_ZP(); V = Cpu6502Read(Ea); goto Cpy;
      case 0xcc: CY(4); A_ABS(); V = Cpu6502Read(Ea);
      Cpy: SETF(CPU6502_C, R->Y >= V); (void)NZ(R->Y - V); break;

      case 0x24: CY(3); A_ZP(); goto Bit;
      case 0x2c: CY(4); A_ABS();
      Bit: V = Cpu6502Read(Ea); SETF(CPU6502_Z, (R->A & V) == 0); R->P = (R->P & 0x3f) | (V & 0xc0); break;

      BRANCH(0x10, !(R->P & CPU6502_N))
      BRANCH(0x30, R->P & CPU6502_N)
      BRANCH(0x50, !(R->P & CPU6502_V))
      BRANCH(0x70, R->P & CPU6502_V)
      BRANCH(0x90, !(R->P & CPU6502_C))
      BRANCH(0xb0, R->P & CPU6502_C)
      BRANCH(0xd0, !(R->P & CPU6502_Z))
      BRANCH(0xf0, R->P & CPU6502_Z)

      case 0x4c: CY(3); A_ABS(); R->PC = Ea; break;
      case 0x6c: CY(5); A_ABS();  // the NMOS part does not carry into the pointer's high byte
         R->PC = Cpu6502Read(Ea) | Cpu6502Read((Ea & 0xff00) | (uint8_t)(Ea + 1)) << 8; break;
      case 0x20: CY(6); A_ABS(); R->PC--; Cpu6502Push(R, R->PC >> 8); Cpu6502Push(R, (uint8_t)R->PC); R->PC = Ea; break;
      case 0x60: Cpu6502Rts(R); break;
      case 0x40: CY(6); R->P = Cpu6502Pull(R) | CPU6502_U; R->P &= ~CPU6502_B;
         Ea = Cpu6502Pull(R); R->PC = Ea | Cpu6502Pull(R) << 8; break;
      case 0x00: R->PC++; Cpu6502Interrupt(R, CPU6502_VECTOR_IRQ, CPU6502_B); break;

      case 0x48: CY(3); Cpu6502Push(R, R->A); break;
      case 0x08: CY(3); Cpu6502Push(R, R->P | CPU6502_B | CPU6502_U); break;
      case 0x68: CY(4); R->A = NZ(Cpu6502Pull(R)); break;
      case 0x28: CY(4); R->P = (Cpu6502Pull(R) & ~CPU6502_B) | CPU6502_U; break;

      case 0xaa: CY(2); R->X = NZ(R->A); break;
      case 0xa8: CY(2); R->Y = NZ(R->A); break;
      case 0x8a: CY(2); R->A = NZ(R->X); break;
      case 0x98: CY(2); R->A = NZ(R->Y); break;
      case 0xba: CY(2); R->X = NZ(R->S); break;
      case 0x9a: CY(2); R->S = R->X; break;
      case 0xe8: CY(2); R->X = NZ(R->X + 1); break;
      case 0xc8: CY(2); R->Y = NZ(R->Y + 1); break;
      case 0xca: CY(2); R->X = NZ(R->X - 1); break;
      case 0x88: CY(2); R->Y = NZ(R->Y - 1); break;

      case 0x18: CY(2); R->P &= ~CPU6502_C; break;
      case 0x38: CY(2); R->P |= CPU6502_C; break;
      case 0x58: CY(2); R->P &= ~CPU6502_I; break;
      case 0x78: CY(2); R->P |= CPU6502_I; break;
      case 0xb8: CY(2); R->P &= ~CPU6502_V; break;
      case 0xd8: CY(2); R->P &= ~CPU6502_D; break;
      case 0xf8: CY(2); R->P |= CPU6502_D; break;
      case 0xea: CY(2); break;

      case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
      case 0x62: case 0x72: case 0x92: case 0xb2: case 0xd2: case 0xf2:
         return Cpu6502_Jam;
      default:
         return Cpu6502_Undefined;
   }
   R->Instructions++;
   return Cpu6502_Budget;

#undef FETCH
#undef CY
#undef SETF
#undef NZ
#undef CROSS
#undef A_ZP
#undef A_ZPX
#undef A_ZPY
#undef A_ABS
#undef A_ABSI
#undef A_INDX
#undef A_INDY
#undef GROUP1
#undef RMW
#undef BRANCH
#undef O_ORA
#undef O_AND
#undef O_EOR
#undef O_LDA
#undef O_CMP
#undef O_ADC
#undef O_SBC
#undef O_ASL
#undef O_LSR
#undef O_ROL
#undef O_ROR
#undef O_DEC
#undef O_INC
}

// https://www.6502.org/tutorials/decimal_mode.html appendix A, the NMOS flags
static inline void Cpu6502Adc(Cpu6502 *R, uint8_t V)
{
   unsigned Carry = R->P & CPU6502_C;
   unsigned Binary = R->A + V + Carry;
   R->P &= ~(CPU6502_C | CPU6502_Z | CPU6502_V | CPU6502_N);
   if ((Binary & 0xff) == 0) R->P |= CPU6502_Z;
   if (!(R->P & CPU6502_D))
   {
      if (Binary > 0xff) R->P |= CPU6502_C;
      if (~(R->A ^ V) & (R->A ^ Binary) & 0x80) R->P |= CPU6502_V;
      R->P |= Binary & 0x80;
      R->A = (uint8_t)Binary;
      return;
   }
   int Low = (R->A & 0x0f) + (V & 0x0f) + Carry;
   if (Low >= 0x0a) Low = ((Low + 0x06) & 0x0f) + 0x10;
   int Total = (R->A & 0xf0) + (V & 0xf0) + Low;
   int Signed = (int8_t)(R->A & 0xf0) + (int8_t)(V & 0xf0) + Low;
   R->P |= Total & 0x80;
   if (Signed < -128 || Signed > 127) R->P |= CPU6502_V;
   if (Total >= 0xa0) Total += 0x60;
   if (Total >= 0x100) R->P |= CPU6502_C;
   R->A = (uint8_t)Total;
}

static inline void Cpu6502Sbc(Cpu6502 *R, uint8_t V)
{
   int Borrow = 1 - (R->P & CPU6502_C);
   int Binary = R->A - V - Borrow;
   R->P &= ~(CPU6502_C | CPU6502_Z | CPU6502_V | CPU6502_N);
   if (Binary >= 0) R->P |= CPU6502_C;
   if ((R->A ^ V) & (R->A ^ Binary) & 0x80) R->P |= CPU6502_V;
   if ((Binary & 0xff) == 0) R->P |= CPU6502_Z;
   R->P |= Binary & 0x80;
   if (!(R->P & CPU6502_D))
   {
      R->A = (uint8_t)Binary;
      return;
   }
   int Low = (R->A & 0x0f) - (V & 0x0f) - Borrow;
   if (Low < 0) Low = ((Low - 0x06) & 0x0f) - 0x10;
   int Difference = (R->A & 0xf0) - (V & 0xf0) + Low;
   if (Difference < 0) Difference -= 0x60;
   R->A = (uint8_t)Difference;
}

// Runs until Cycles reaches Until or a JAM or undefined opcode, taking an NMI or IRQ between
// instructions. On a stop other than the budget, PC is on the opcode that stopped it.
static CPU6502_FN __attribute__((unused)) Cpu6502Stop Cpu6502Run(Cpu6502 *Cpu, uint64_t Until)
{
   Cpu6502 R = *Cpu;
   Cpu6502Stop Stop = Cpu6502_Budget;
   while (R.Cycles < Until)
   {
      if (Cpu6502TakeNmi()) Cpu6502Interrupt(&R, CPU6502_VECTOR_NMI, 0);
      else if (!(R.P & CPU6502_I) && Cpu6502IrqLine()) Cpu6502Interrupt(&R, CPU6502_VECTOR_IRQ, 0);
      uint8_t Op = Cpu6502Read(R.PC++);
      Stop = Cpu6502Op(&R, Op);
      if (Stop != Cpu6502_Budget)
      {
         R.PC--;
         break;
      }
   }
   *Cpu = R;
   return Stop;
}

// Runs one instruction whose opcode is Op rather than the byte at PC, which a caller uses to run
// the instruction it patched over with a JAM. PC is on the patched byte, and moves past it.
static CPU6502_FN __attribute__((unused)) Cpu6502Stop Cpu6502Exec(Cpu6502 *Cpu, uint8_t Op)
{
   Cpu6502 R = *Cpu;
   R.PC++;
   Cpu6502Stop Stop = Cpu6502Op(&R, Op);
   if (Stop != Cpu6502_Budget) R.PC--;
   *Cpu = R;
   return Stop;
}

#endif
