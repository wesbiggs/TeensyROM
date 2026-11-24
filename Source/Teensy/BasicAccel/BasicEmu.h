// SPDX-License-Identifier: MIT
//
// A C64 run on the Teensy rather than by its own CPU: the BASIC and KERNAL ROMs run in
// Cpu6502.h against a copy of the C64's RAM, and only what the C64's chips must see crosses the
// bus. While it runs, the real 6510 is held, as the REU holds it.
//
//   - RAM is the copy here. Writes mark a dirty bitmap; BasicEmuFlushVideo writes the dirty bytes
//     the VIC can see (screen, character set or bitmap, sprites) to the C64, about once a frame.
//   - I/O ($D000-$DFFF while the 6510's port maps it in) goes to the bus, one access for each the
//     6502 makes, through two functions the includer defines (DMA on the board):
//
//       static inline uint8_t BasicEmuBusRead(uint16_t Addr);
//       static inline void BasicEmuBusWrite(uint16_t Addr, uint8_t Value);
//
//     The includer also defines Cpu6502IrqLine and Cpu6502TakeNmi (see Cpu6502.h): with the
//     6510 held, nothing else answers the CIAs' and VIC's interrupts.
//   - The 6510's own port, $00/$01, is inside the CPU, where DMA cannot reach it, so it is
//     modeled here and decides the banking.
//   - BasicEmuInit patches a JAM over the first byte of each float routine CbmFloat.h ports, in
//     the copy of the ROM. Reaching one calls the C port with the registers it could not see
//     from the cartridge, and returns from the routine; if the port declines (the ROM raises an
//     error itself), the patched instruction runs and the ROM carries on.
//
// No cartridge is mapped (GAME and EXROM high). Nothing here keeps the C64's timing.
#ifndef BASIC_EMU_H
#define BASIC_EMU_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "CbmFloat.h"
#include "Cpu6502.h"

// Where the code and the 92 KB of state go: FLASHMEM and DMAMEM (RAM2) on the board
#ifndef BASIC_EMU_FN
#define BASIC_EMU_FN
#endif
#ifndef BASIC_EMU_DATA
#define BASIC_EMU_DATA
#endif

static inline uint8_t BasicEmuBusRead(uint16_t Addr);
static inline void BasicEmuBusWrite(uint16_t Addr, uint8_t Value);

typedef enum {
   BasicEmuHook_FAddT, BasicEmuHook_FSubT, BasicEmuHook_FMultT, BasicEmuHook_FDivT,
   BasicEmuHook_FAdd, BasicEmuHook_FSub, BasicEmuHook_FMult, BasicEmuHook_FDiv,
   BasicEmuHook_Fin, BasicEmuHook_Qint, BasicEmuHook_Ayint,
   BasicEmuHook_Count
} BasicEmuHookId;

// the entries tools/basic-accel/float-cases.mjs ROUTINES names
static const uint16_t BasicEmuHookAddr[BasicEmuHook_Count] = {
   0xb86a, 0xb853, 0xba2b, 0xbb12, 0xb867, 0xb850, 0xba28, 0xbb0f, 0xbcf3, 0xbc9b, 0xb1bf,
};

typedef struct {
   uint8_t Ram[0x10000];
   uint8_t Basic[0x2000], Kernal[0x2000], Chargen[0x1000];
   uint8_t Dirty[0x10000 / 8];
   uint8_t IoLast[0x1000];  // the last value the 6502 wrote to each I/O address
   uint8_t Ddr, Port;       // the 6510's $00 and $01
   uint8_t Original[BasicEmuHook_Count];  // the ROM byte each JAM replaced
   uint32_t HookCalls[BasicEmuHook_Count], HookNative[BasicEmuHook_Count];
} BasicEmuState;

BASIC_EMU_DATA static BasicEmuState BasicEmu;

// The port's lines as the PLA sees them: an input bit is pulled up
static inline uint8_t BasicEmuLines(void) { return (uint8_t)((BasicEmu.Port & BasicEmu.Ddr) | ~BasicEmu.Ddr); }
static inline bool BasicEmuIoVisible(uint8_t L) { return (L & 3) && (L & 4); }

static inline void BasicEmuMarkDirty(uint16_t Addr) { BasicEmu.Dirty[Addr >> 3] |= (uint8_t)(1 << (Addr & 7)); }

// A read without side effects, for the hooks' operands: false where it would reach I/O
static inline bool BasicEmuPeek(uint16_t Addr, uint8_t *Value)
{
   uint8_t L = BasicEmuLines();
   if (Addr >= 0xa000 && Addr < 0xc000 && (L & 3) == 3) *Value = BasicEmu.Basic[Addr - 0xa000];
   else if (Addr >= 0xe000 && (L & 2)) *Value = BasicEmu.Kernal[Addr - 0xe000];
   else if (Addr >= 0xd000 && Addr < 0xe000 && (L & 3))
   {
      if (L & 4) return false;
      *Value = BasicEmu.Chargen[Addr - 0xd000];
   }
   else if (Addr < 2) *Value = Addr ? (uint8_t)((BasicEmu.Port & BasicEmu.Ddr) | (~BasicEmu.Ddr & 0x17)) : BasicEmu.Ddr;
   else *Value = BasicEmu.Ram[Addr];
   return true;
}

static inline uint8_t Cpu6502Read(uint16_t Addr)
{
   // RAM below BASIC is most reads: the program, its variables, zero page and the stack
   if (Addr < 0xa000 && Addr >= 2) return BasicEmu.Ram[Addr];
   uint8_t Value;
   if (BasicEmuPeek(Addr, &Value)) return Value;
   return BasicEmuBusRead(Addr);
}

static inline void Cpu6502Write(uint16_t Addr, uint8_t Value)
{
   if (Addr >= 0xd000 && Addr < 0xe000 && BasicEmuIoVisible(BasicEmuLines()))
   {
      BasicEmu.IoLast[Addr - 0xd000] = Value;
      BasicEmuBusWrite(Addr, Value);
      return;
   }
   if (Addr < 2)
   {
      if (Addr) BasicEmu.Port = Value;
      else BasicEmu.Ddr = Value;
      return;
   }
   BasicEmu.Ram[Addr] = Value;
   BasicEmuMarkDirty(Addr);
}

static inline void Cpu6502Modify(uint16_t Addr, uint8_t Old, uint8_t New)
{
   if (Addr >= 0xd000 && Addr < 0xe000 && BasicEmuIoVisible(BasicEmuLines())) BasicEmuBusWrite(Addr, Old);
   Cpu6502Write(Addr, New);
}

// Ram, Basic, Kernal and Chargen as the C64 holds them; Hooks is a mask of 1 << BasicEmuHookId
static BASIC_EMU_FN __attribute__((unused)) void BasicEmuInit(const uint8_t Ram[0x10000], const uint8_t Basic[0x2000],
                                                             const uint8_t Kernal[0x2000], const uint8_t Chargen[0x1000],
                                                             uint32_t Hooks)
{
   memcpy(BasicEmu.Ram, Ram, sizeof BasicEmu.Ram);
   memcpy(BasicEmu.Basic, Basic, sizeof BasicEmu.Basic);
   memcpy(BasicEmu.Kernal, Kernal, sizeof BasicEmu.Kernal);
   memcpy(BasicEmu.Chargen, Chargen, sizeof BasicEmu.Chargen);
   memset(BasicEmu.Dirty, 0, sizeof BasicEmu.Dirty);
   memset(BasicEmu.IoLast, 0, sizeof BasicEmu.IoLast);
   memset(BasicEmu.HookCalls, 0, sizeof BasicEmu.HookCalls);
   memset(BasicEmu.HookNative, 0, sizeof BasicEmu.HookNative);
   BasicEmu.Ddr = 0x2f;
   BasicEmu.Port = 0x37;
   for (int i = 0; i < BasicEmuHook_Count; i++)
   {
      uint8_t *At = &BasicEmu.Basic[BasicEmuHookAddr[i] - 0xa000];
      BasicEmu.Original[i] = *At;
      if (Hooks & (1u << i)) *At = 0x02;  // JAM
   }
}

// The hooks' registers and zero page in and out
static inline void BasicEmuToCbm(const Cpu6502 *Cpu, CbmCpu *C)
{
   memcpy(C->Zp, BasicEmu.Ram, 256);
   C->A = Cpu->A;
   C->X = Cpu->X;
   C->Y = Cpu->Y;
   C->P = Cpu->P;
   C->Set = 0;
}

static inline void BasicEmuFromCbm(Cpu6502 *Cpu, const CbmCpu *C)
{
   for (int i = 2; i < 256; i++)
      if (BasicEmu.Ram[i] != C->Zp[i])
      {
         BasicEmu.Ram[i] = C->Zp[i];
         BasicEmuMarkDirty((uint16_t)i);
      }
   if (C->Set & CBM_SET_A) Cpu->A = C->A;
   if (C->Set & CBM_SET_X) Cpu->X = C->X;
   if (C->Set & CBM_SET_Y) Cpu->Y = C->Y;
   uint8_t Flags = C->Set & (CBM_FLAG_C | CBM_FLAG_Z | CBM_FLAG_V | CBM_FLAG_N);
   Cpu->P = (uint8_t)((Cpu->P & ~Flags) | (C->P & Flags));
}

// FIN's text: from TXTPTR up to whatever stops a side-effect-free read, at most 255 bytes
static inline unsigned BasicEmuText(uint16_t From, uint8_t Text[255])
{
   unsigned Len = 0;
   while (Len < 255 && BasicEmuPeek((uint16_t)(From + Len), &Text[Len])) Len++;
   return Len;
}

// Answers the hook at PC, or runs the instruction it patched over. False if PC is not a hook.
static BASIC_EMU_FN bool BasicEmuHook(Cpu6502 *Cpu)
{
   if (Cpu->PC < 0xa000 || Cpu->PC >= 0xc000 || (BasicEmuLines() & 3) != 3) return false;
   unsigned Id = 0;
   while (Id < BasicEmuHook_Count && BasicEmuHookAddr[Id] != Cpu->PC) Id++;
   if (Id >= BasicEmuHook_Count || BasicEmu.Basic[Cpu->PC - 0xa000] != 0x02) return false;

   static CbmCpu C;
   BasicEmuToCbm(Cpu, &C);
   CbmFloatResult Result = CbmFloat_Native;
   uint8_t Operand[5];
   uint16_t At = (uint16_t)(Cpu->A | Cpu->Y << 8);
   bool OperandOk = true;
   if (Id >= BasicEmuHook_FAdd && Id <= BasicEmuHook_FDiv)
      for (int i = 0; i < 5; i++) OperandOk &= BasicEmuPeek((uint16_t)(At + i), &Operand[i]);
   switch (Id)
   {
      case BasicEmuHook_FAddT: Result = CbmFAddT(&C); break;
      case BasicEmuHook_FSubT: Result = CbmFSubT(&C); break;
      case BasicEmuHook_FMultT: Result = CbmFMultT(&C); break;
      case BasicEmuHook_FDivT: Result = CbmFDivT(&C); break;
      case BasicEmuHook_FAdd: if (OperandOk) Result = CbmFAdd(&C, Operand); break;
      case BasicEmuHook_FSub: if (OperandOk) Result = CbmFSub(&C, Operand); break;
      case BasicEmuHook_FMult: if (OperandOk) Result = CbmFMult(&C, Operand); break;
      case BasicEmuHook_FDiv: if (OperandOk) Result = CbmFDiv(&C, Operand); break;
      case BasicEmuHook_Fin:
      {
         uint8_t Bytes[255];
         CbmText Text = { Bytes, BasicEmuText((uint16_t)(BasicEmu.Ram[0x7a] | BasicEmu.Ram[0x7b] << 8), Bytes), 0 };
         Result = CbmFin(&C, &Text);
         break;
      }
      case BasicEmuHook_Qint: Result = CbmQint(&C); break;
      case BasicEmuHook_Ayint: Result = CbmAyint(&C); break;
   }
   BasicEmu.HookCalls[Id]++;
   if (Result == CbmFloat_Done)
   {
      BasicEmuFromCbm(Cpu, &C);
      Cpu6502Rts(Cpu);
      return true;
   }
   BasicEmu.HookNative[Id]++;
   Cpu6502Exec(Cpu, BasicEmu.Original[Id]);
   return true;
}

// Runs to Until, answering the hooks. Stops early, with PC on it, at a JAM that is not a hook
// or an undocumented opcode.
static BASIC_EMU_FN __attribute__((unused)) Cpu6502Stop BasicEmuRun(Cpu6502 *Cpu, uint64_t Until)
{
   for (;;)
   {
      Cpu6502Stop Stop = Cpu6502Run(Cpu, Until);
      if (Stop != Cpu6502_Jam || !BasicEmuHook(Cpu)) return Stop;
   }
}

// Writes the dirty bytes in Addr..Addr+Len-1 to the C64 and clears them; returns how many
static inline unsigned BasicEmuFlushRange(uint32_t Addr, uint32_t Len)
{
   unsigned Count = 0;
   for (uint32_t End = Addr + Len; Addr < End && Addr < 0x10000;)
   {
      uint8_t *D = &BasicEmu.Dirty[Addr >> 3];
      if (!*D && (Addr & 7) == 0 && Addr + 8 <= End)
      {
         Addr += 8;
         continue;
      }
      uint8_t Bit = (uint8_t)(1 << (Addr & 7));
      if (*D & Bit)
      {
         *D &= (uint8_t)~Bit;
         BasicEmuBusWrite((uint16_t)Addr, BasicEmu.Ram[Addr]);
         Count++;
      }
      Addr++;
   }
   return Count;
}

// The VIC sees RAM in its 16K bank, except the character ROM at $1000-$1FFF of banks 0 and 2
static inline unsigned BasicEmuFlushVic(uint32_t Bank, uint32_t Offset, uint32_t Len)
{
   uint32_t End = Offset + Len;
   if ((Bank != 0 && Bank != 0x8000) || End <= 0x1000 || Offset >= 0x2000) return BasicEmuFlushRange(Bank + Offset, Len);
   unsigned Count = 0;
   if (Offset < 0x1000) Count += BasicEmuFlushRange(Bank + Offset, 0x1000 - Offset);
   if (End > 0x2000) Count += BasicEmuFlushRange(Bank + 0x2000, End - 0x2000);
   return Count;
}

// What the VIC displays, by the registers the 6502 last wrote: the screen, the character set or
// bitmap, and the enabled sprites. Returns how many bytes it wrote.
static BASIC_EMU_FN __attribute__((unused)) unsigned BasicEmuFlushVideo(void)
{
   const uint8_t *Io = BasicEmu.IoLast;
   uint32_t Bank = (uint32_t)(3 - (Io[0xd00] & 3)) << 14;  // $DD00, the bank's bits inverted
   uint8_t D018 = Io[0x018];
   uint32_t Screen = (uint32_t)(D018 >> 4) << 10;
   unsigned Count = BasicEmuFlushVic(Bank, Screen, 0x400);
   if (Io[0x011] & 0x20) Count += BasicEmuFlushVic(Bank, (D018 & 8) ? 0x2000 : 0, 0x2000);
   else Count += BasicEmuFlushVic(Bank, (uint32_t)((D018 >> 1) & 7) << 11, 0x800);
   for (int i = 0; i < 8; i++)
      if (Io[0x015] & (1 << i)) Count += BasicEmuFlushVic(Bank, (uint32_t)BasicEmu.Ram[Bank + Screen + 0x3f8 + i] << 6, 64);
   return Count;
}

// Every dirty byte, as handing the C64 back to its own CPU needs; returns how many
static BASIC_EMU_FN __attribute__((unused)) unsigned BasicEmuFlushAll(void) { return BasicEmuFlushRange(2, 0x10000 - 2); }

#endif
