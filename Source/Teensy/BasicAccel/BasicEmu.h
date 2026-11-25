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

// Where the code and the tables go: FLASHMEM and PROGMEM on the board. The 92 KB of state is a
// static, or with BASIC_EMU_ON_HEAP defined, wherever the includer points BasicEmuHeap.
#ifndef BASIC_EMU_FN
#define BASIC_EMU_FN
#endif
#ifndef BASIC_EMU_CONST
#define BASIC_EMU_CONST
#endif

CPU6502_INLINE uint8_t BasicEmuBusRead(uint16_t Addr);
CPU6502_INLINE void BasicEmuBusWrite(uint16_t Addr, uint8_t Value);

typedef enum {
   BasicEmuHook_FAddT, BasicEmuHook_FSubT, BasicEmuHook_FMultT, BasicEmuHook_FDivT,
   BasicEmuHook_FAdd, BasicEmuHook_FSub, BasicEmuHook_FMult, BasicEmuHook_FDiv,
   BasicEmuHook_Fin, BasicEmuHook_Qint, BasicEmuHook_Ayint,
   BasicEmuHook_Count
} BasicEmuHookId;

// the entries tools/basic-accel/float-cases.mjs ROUTINES names
static const uint16_t BasicEmuHookAddr[BasicEmuHook_Count] BASIC_EMU_CONST = {
   0xb86a, 0xb853, 0xba2b, 0xbb12, 0xb867, 0xb850, 0xba28, 0xbb0f, 0xbcf3, 0xbc9b, 0xb1bf,
};

typedef struct {
   const uint8_t *ReadMap[256];  // each page as a read sees it, by the port; NULL where it is I/O
   uint8_t Ram[0x10000];         // $00 and $01 hold what the port reads, for ReadMap
   uint8_t Basic[0x2000], Kernal[0x2000], Chargen[0x1000];
   uint8_t Dirty[0x10000 / 8];
   uint8_t IoLast[0x1000];  // the last value the 6502 wrote to each I/O address
   uint8_t Ddr, Port;       // the 6510's $00 and $01
   uint8_t Original[BasicEmuHook_Count];  // the ROM byte each JAM replaced
   uint32_t HookCalls[BasicEmuHook_Count], HookNative[BasicEmuHook_Count];
   CbmCpu Cbm;  // a hook's registers and zero page
} BasicEmuState;

#ifdef BASIC_EMU_ON_HEAP
static BasicEmuState *BasicEmuHeap;
#define BasicEmu (*BasicEmuHeap)
#else
static BasicEmuState BasicEmu;
#endif

// The port's lines as the PLA sees them: an input bit is pulled up
CPU6502_INLINE uint8_t BasicEmuLines(void) { return (uint8_t)((BasicEmu.Port & BasicEmu.Ddr) | ~BasicEmu.Ddr); }
CPU6502_INLINE bool BasicEmuIoVisible(uint8_t L) { return (L & 3) && (L & 4); }

CPU6502_INLINE void BasicEmuMarkDirty(uint16_t Addr) { BasicEmu.Dirty[Addr >> 3] |= (uint8_t)(1 << (Addr & 7)); }

// ReadMap and $00/$01 from the port, after a write to either
static BASIC_EMU_FN void BasicEmuRemap(void)
{
   uint8_t L = BasicEmuLines();
   for (int Page = 0; Page < 256; Page++)
   {
      const uint8_t *At = &BasicEmu.Ram[Page << 8];
      if (Page >= 0xa0 && Page < 0xc0 && (L & 3) == 3) At = &BasicEmu.Basic[(Page - 0xa0) << 8];
      else if (Page >= 0xe0 && (L & 2)) At = &BasicEmu.Kernal[(Page - 0xe0) << 8];
      else if (Page >= 0xd0 && Page < 0xe0 && (L & 3)) At = (L & 4) ? NULL : &BasicEmu.Chargen[(Page - 0xd0) << 8];
      BasicEmu.ReadMap[Page] = At;
   }
   BasicEmu.Ram[0] = BasicEmu.Ddr;
   BasicEmu.Ram[1] = (uint8_t)((BasicEmu.Port & BasicEmu.Ddr) | (~BasicEmu.Ddr & 0x17));
}

// A read without side effects, for the hooks' operands: false where it would reach I/O
CPU6502_INLINE bool BasicEmuPeek(uint16_t Addr, uint8_t *Value)
{
   const uint8_t *Page = BasicEmu.ReadMap[Addr >> 8];
   if (!Page) return false;
   *Value = Page[Addr & 0xff];
   return true;
}

CPU6502_INLINE uint8_t Cpu6502Read(uint16_t Addr)
{
   const uint8_t *Page = BasicEmu.ReadMap[Addr >> 8];
   if (Page) return Page[Addr & 0xff];
   return BasicEmuBusRead(Addr);
}

// Writes to the port, and to $D000-$DFFF, which is I/O or RAM by the port
static BASIC_EMU_FN __attribute__((noinline)) void BasicEmuWriteSlow(uint16_t Addr, uint8_t Value)
{
   if (Addr < 2)
   {
      if (Addr) BasicEmu.Port = Value;
      else BasicEmu.Ddr = Value;
      BasicEmuRemap();
      return;
   }
   if (!BasicEmu.ReadMap[Addr >> 8])
   {
      BasicEmu.IoLast[Addr - 0xd000] = Value;
      BasicEmuBusWrite(Addr, Value);
      return;
   }
   BasicEmu.Ram[Addr] = Value;
   BasicEmuMarkDirty(Addr);
}

CPU6502_INLINE void Cpu6502Write(uint16_t Addr, uint8_t Value)
{
   if (Addr >= 2 && (uint16_t)(Addr - 0xd000) >= 0x1000)
   {
      BasicEmu.Ram[Addr] = Value;
      BasicEmuMarkDirty(Addr);
   }
   else BasicEmuWriteSlow(Addr, Value);
}

CPU6502_INLINE void Cpu6502Modify(uint16_t Addr, uint8_t Old, uint8_t New)
{
   if ((uint16_t)(Addr - 0xd000) < 0x1000 && !BasicEmu.ReadMap[Addr >> 8]) BasicEmuBusWrite(Addr, Old);
   Cpu6502Write(Addr, New);
}

// Ram, Basic, Kernal and Chargen as the C64 holds them, Ram and Chargen NULL for zeros; Hooks is
// a mask of 1 << BasicEmuHookId
static BASIC_EMU_FN __attribute__((unused)) void BasicEmuInit(const uint8_t Ram[0x10000], const uint8_t Basic[0x2000],
                                                             const uint8_t Kernal[0x2000], const uint8_t Chargen[0x1000],
                                                             uint32_t Hooks)
{
   if (Ram) memcpy(BasicEmu.Ram, Ram, sizeof BasicEmu.Ram);
   else memset(BasicEmu.Ram, 0, sizeof BasicEmu.Ram);
   memcpy(BasicEmu.Basic, Basic, sizeof BasicEmu.Basic);
   memcpy(BasicEmu.Kernal, Kernal, sizeof BasicEmu.Kernal);
   if (Chargen) memcpy(BasicEmu.Chargen, Chargen, sizeof BasicEmu.Chargen);
   else memset(BasicEmu.Chargen, 0, sizeof BasicEmu.Chargen);
   memset(BasicEmu.Dirty, 0, sizeof BasicEmu.Dirty);
   memset(BasicEmu.IoLast, 0, sizeof BasicEmu.IoLast);
   memset(BasicEmu.HookCalls, 0, sizeof BasicEmu.HookCalls);
   memset(BasicEmu.HookNative, 0, sizeof BasicEmu.HookNative);
   BasicEmu.Ddr = 0x2f;
   BasicEmu.Port = 0x37;
   BasicEmuRemap();
   for (int i = 0; i < BasicEmuHook_Count; i++)
   {
      uint8_t *At = &BasicEmu.Basic[BasicEmuHookAddr[i] - 0xa000];
      BasicEmu.Original[i] = *At;
      if (Hooks & (1u << i)) *At = 0x02;  // JAM
   }
}

// The hooks' registers and zero page in and out
static BASIC_EMU_FN void BasicEmuToCbm(const Cpu6502 *Cpu, CbmCpu *C)
{
   memcpy(C->Zp, BasicEmu.Ram, 256);
   C->A = Cpu->A;
   C->X = Cpu->X;
   C->Y = Cpu->Y;
   C->P = Cpu->P;
   C->Set = 0;
}

static BASIC_EMU_FN void BasicEmuFromCbm(Cpu6502 *Cpu, const CbmCpu *C)
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
static BASIC_EMU_FN unsigned BasicEmuText(uint16_t From, uint8_t Text[255])
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

   CbmCpu *C = &BasicEmu.Cbm;
   BasicEmuToCbm(Cpu, C);
   CbmFloatResult Result = CbmFloat_Native;
   uint8_t Operand[5];
   uint16_t At = (uint16_t)(Cpu->A | Cpu->Y << 8);
   bool OperandOk = true;
   if (Id >= BasicEmuHook_FAdd && Id <= BasicEmuHook_FDiv)
      for (int i = 0; i < 5; i++) OperandOk &= BasicEmuPeek((uint16_t)(At + i), &Operand[i]);
   switch (Id)
   {
      case BasicEmuHook_FAddT: Result = CbmFAddT(C); break;
      case BasicEmuHook_FSubT: Result = CbmFSubT(C); break;
      case BasicEmuHook_FMultT: Result = CbmFMultT(C); break;
      case BasicEmuHook_FDivT: Result = CbmFDivT(C); break;
      case BasicEmuHook_FAdd: if (OperandOk) Result = CbmFAdd(C, Operand); break;
      case BasicEmuHook_FSub: if (OperandOk) Result = CbmFSub(C, Operand); break;
      case BasicEmuHook_FMult: if (OperandOk) Result = CbmFMult(C, Operand); break;
      case BasicEmuHook_FDiv: if (OperandOk) Result = CbmFDiv(C, Operand); break;
      case BasicEmuHook_Fin:
      {
         uint8_t Bytes[255];
         CbmText Text = { Bytes, BasicEmuText((uint16_t)(BasicEmu.Ram[0x7a] | BasicEmu.Ram[0x7b] << 8), Bytes), 0 };
         Result = CbmFin(C, &Text);
         break;
      }
      case BasicEmuHook_Qint: Result = CbmQint(C); break;
      case BasicEmuHook_Ayint: Result = CbmAyint(C); break;
   }
   BasicEmu.HookCalls[Id]++;
   if (Result == CbmFloat_Done)
   {
      BasicEmuFromCbm(Cpu, C);
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

// Writes the dirty bytes in Addr..Addr+Len-1 to the C64 and clears them; returns how many.
// RAM under I/O stays dirty: the held 6510's port maps I/O there, so DMA would write the chip.
// Only giving the 6510 back, with its port set to RAM, reaches it.
static BASIC_EMU_FN unsigned BasicEmuFlushRange(uint32_t Addr, uint32_t Len)
{
   unsigned Count = 0;
   for (uint32_t End = Addr + Len; Addr < End && Addr < 0x10000;)
   {
      if (Addr >= 0xd000 && Addr < 0xe000)
      {
         Addr = 0xe000;
         continue;
      }
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
static BASIC_EMU_FN unsigned BasicEmuFlushVic(uint32_t Bank, uint32_t Offset, uint32_t Len)
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

// Every dirty byte DMA can reach, as handing the C64 back to its own CPU needs; returns how many
static BASIC_EMU_FN __attribute__((unused)) unsigned BasicEmuFlushAll(void) { return BasicEmuFlushRange(2, 0x10000 - 2); }

#endif
