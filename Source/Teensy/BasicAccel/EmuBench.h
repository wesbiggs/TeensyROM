// SPDX-License-Identifier: MIT
//
// BasicEmu.h running a BASIC program from a cold start, against a model of the C64 on the other
// side of the bus, and timing it. tools/basic-accel/emu_model.c runs it on the host and
// IOH_Debug's emulator benchmark on a TR+, so both do the same work.
//
// It boots the ROMs, types RUN at BASIC's first prompt, and runs until the program writes 1 to
// $033C (SinePlot.bas does when it has drawn).
//
// The model's clock is the C64's, in its cycles. From RUN, the 6502 runs Speed native cycles per
// C64 cycle; each bus read stalls it ReadCost cycles and each bus write (an I/O write, or a dirty
// byte flushed to the VIC once a frame) costs WriteCost. HookCost is what one answered float
// routine costs. Native models the C64's own CPU instead: speed 1, the bus free, no flush, and
// the cycles the VIC takes on badlines. The boot always runs at speed 1: faster, the KERNAL
// clears the screen in under a frame, its PAL check ($FF5E) finds the raster latch for line 311
// unset, and TI runs NTSC-slow.
//
// The C64's side is what the KERNAL and BASIC need: CIA1's timer A and its IRQ (the jiffy clock),
// the VIC's raster counter and raster latch, and every other register holding what was written.
// Nothing reaches a real C64; the RAM is the result.
//
// The includer defines, besides what BasicEmu.h asks for:
//
//   CPU6502_INLINE uint64_t EmuBenchNowNs(void);   // a clock for the time limit and RunNs
#ifndef EMU_BENCH_H
#define EMU_BENCH_H

#include "BasicEmu.h"

#ifndef EMU_BENCH_FN
#define EMU_BENCH_FN
#endif

CPU6502_INLINE uint64_t EmuBenchNowNs(void);

#define EMU_BENCH_PAL_LINES 312
#define EMU_BENCH_PAL_LINE_CYCLES 63
#define EMU_BENCH_PAL_FRAME (EMU_BENCH_PAL_LINES * EMU_BENCH_PAL_LINE_CYCLES)
#define EMU_BENCH_BADLINE_STEAL (25 * 40)  // cycles a frame the VIC takes for character pointers
#define EMU_BENCH_DONE 0x33c               // the program writes 1 here when it is done

typedef struct {
   double Speed, ReadCost, WriteCost, HookCost;
   bool Native;
   double MaxC64Cycles;  // give up after this much of the model's time
   uint64_t MaxNs;       // or this much of EmuBenchNowNs's
} EmuBenchOptions;

typedef enum {
   EmuBench_Done,
   EmuBench_TimeLimit,
   EmuBench_Stopped,  // a JAM or undocumented opcode at StopPc
} EmuBenchStatus;

typedef struct {
   EmuBenchStatus Status;
   uint16_t StopPc;
   uint64_t RunNs;                     // RUN to done, by EmuBenchNowNs
   double RunCycles;                   // RUN to done, in the model's C64 cycles
   uint64_t Instructions, CpuCycles;   // the 6502's, from RUN
   uint32_t RunJiffies;                // TI from RUN
   uint32_t Jiffies;                   // what the program measured itself, at 830/831
   uint32_t HookCalls, HookNative;
   uint32_t Bitmap, Screen;            // checksums of $2000-$3F3F and $0400-$07E7
} EmuBenchResult;

typedef struct {
   double Time;          // C64 cycles since reset
   double CiaCycles;     // whole cycles of Time the timer has counted
   uint8_t Io[0x1000];
   int32_t TimerA;
   uint16_t LatchA;
   uint8_t Icr, IcrMask;
   uint8_t VicIrq;
   uint32_t LastLine;    // raster lines since reset, at the last update
   double ReadCost, WriteCost;
   uint64_t Reads, Writes, Flushed, Frames;
   uint64_t ReadsBy[16], WritesBy[16];  // by I/O page, $D0 to $DF
   uint64_t ClockTicks;  // for the includer's EmuBenchNowNs, if it keeps state
   uint32_t ClockLast;
} EmuBenchState;

#ifdef BASIC_EMU_ON_HEAP
static EmuBenchState *EmuBenchHeap;
#define EmuBenchC64 (*EmuBenchHeap)
#else
static EmuBenchState EmuBenchC64;
#endif

// The VIC's raster, and its latch for the compare line
static EMU_BENCH_FN void EmuBenchVicUpdate(void)
{
   uint32_t Lines = (uint32_t)(EmuBenchC64.Time / EMU_BENCH_PAL_LINE_CYCLES);
   uint32_t Compare = EmuBenchC64.Io[0x012] | (EmuBenchC64.Io[0x011] & 0x80) << 1;
   if (Compare < EMU_BENCH_PAL_LINES && Lines != EmuBenchC64.LastLine)
   {
      uint32_t Crossed = Lines - EmuBenchC64.LastLine;
      uint32_t From = (EmuBenchC64.LastLine + 1) % EMU_BENCH_PAL_LINES;
      if (Crossed >= EMU_BENCH_PAL_LINES || (Compare - From + EMU_BENCH_PAL_LINES) % EMU_BENCH_PAL_LINES < Crossed)
         EmuBenchC64.VicIrq |= 1;
   }
   EmuBenchC64.LastLine = Lines;
}

// The timer counts whole cycles of the clock, so the fractions the slices leave add up. Every
// conversion between double and an integer here is 32-bit: the M7's FPU does those itself, and
// 64-bit ones are libgcc calls, which the Teensy's linker puts in ITCM.
static EMU_BENCH_FN void EmuBenchCiaUpdate(void)
{
   uint32_t Elapsed = (uint32_t)(EmuBenchC64.Time - EmuBenchC64.CiaCycles);
   EmuBenchC64.CiaCycles += Elapsed;
   if (!(EmuBenchC64.Io[0xc0e] & 1)) return;
   EmuBenchC64.TimerA -= (int32_t)Elapsed;
   while (EmuBenchC64.TimerA < 0)
   {
      EmuBenchC64.Icr |= 1;
      if (EmuBenchC64.Io[0xc0e] & 8)
      {
         EmuBenchC64.Io[0xc0e] &= ~1;
         EmuBenchC64.TimerA = EmuBenchC64.LatchA;
         break;
      }
      EmuBenchC64.TimerA += EmuBenchC64.LatchA + 1;
   }
}

static EMU_BENCH_FN void EmuBenchAdvance(double Cycles)
{
   EmuBenchC64.Time += Cycles;
   EmuBenchCiaUpdate();
   EmuBenchVicUpdate();
}

CPU6502_INLINE bool Cpu6502IrqLine(void)
{
   return (EmuBenchC64.Icr & EmuBenchC64.IcrMask & 0x1f) || (EmuBenchC64.VicIrq & EmuBenchC64.Io[0x01a] & 0x0f);
}

CPU6502_INLINE bool Cpu6502TakeNmi(void) { return false; }

static EMU_BENCH_FN uint8_t EmuBenchRead(uint16_t Addr)
{
   EmuBenchC64.Reads++;
   EmuBenchC64.ReadsBy[(Addr >> 8) & 15]++;
   EmuBenchAdvance(EmuBenchC64.ReadCost);
   uint16_t R = Addr & 0xfff;
   if (R < 0x400)
   {
      R &= 0x3f;
      uint32_t Line = EmuBenchC64.LastLine % EMU_BENCH_PAL_LINES;
      if (R == 0x11) return (uint8_t)((EmuBenchC64.Io[0x011] & 0x7f) | (Line >> 1 & 0x80));
      if (R == 0x12) return (uint8_t)Line;
      if (R == 0x19)
         return (uint8_t)(EmuBenchC64.VicIrq | ((EmuBenchC64.VicIrq & EmuBenchC64.Io[0x01a] & 0x0f) ? 0x80 : 0) | 0x70);
      return EmuBenchC64.Io[R];
   }
   if (R >= 0x800 && R < 0xc00) return EmuBenchC64.Io[R] | 0xf0;  // color RAM is four bits
   if (R >= 0xc00 && R < 0xd00)
   {
      R = 0xc00 | (R & 0x0f);
      if (R == 0xc00 || R == 0xc01) return 0xff;  // no key down, no joystick
      if (R == 0xc04) return (uint8_t)EmuBenchC64.TimerA;
      if (R == 0xc05) return (uint8_t)(EmuBenchC64.TimerA >> 8);
      if (R == 0xc0d)
      {
         uint8_t Value = (uint8_t)(EmuBenchC64.Icr | ((EmuBenchC64.Icr & EmuBenchC64.IcrMask & 0x1f) ? 0x80 : 0));
         EmuBenchC64.Icr = 0;
         return Value;
      }
   }
   return EmuBenchC64.Io[R];
}

static EMU_BENCH_FN void EmuBenchWrite(uint16_t Addr, uint8_t Value)
{
   EmuBenchC64.Writes++;
   if (Addr >= 0xd000 && Addr < 0xe000) EmuBenchC64.WritesBy[(Addr >> 8) & 15]++;
   EmuBenchAdvance(EmuBenchC64.WriteCost);
   if (Addr < 0xd000 || Addr >= 0xe000) return;  // a flushed RAM byte: the RAM is BasicEmu's
   uint16_t R = Addr & 0xfff;
   if (R < 0x400) R &= 0x3f;
   if (R >= 0xc00 && R < 0xd00) R = 0xc00 | (R & 0x0f);
   if (R == 0x019)
   {
      EmuBenchC64.VicIrq &= (uint8_t)~(Value & 0x0f);
      return;
   }
   if (R == 0xc0d)
   {
      if (Value & 0x80) EmuBenchC64.IcrMask |= Value & 0x1f;
      else EmuBenchC64.IcrMask &= (uint8_t)~(Value & 0x1f);
      return;
   }
   if (R == 0xc04) EmuBenchC64.LatchA = (EmuBenchC64.LatchA & 0xff00) | Value;
   if (R == 0xc05)
   {
      EmuBenchC64.LatchA = (uint16_t)((EmuBenchC64.LatchA & 0xff) | Value << 8);
      if (!(EmuBenchC64.Io[0xc0e] & 1)) EmuBenchC64.TimerA = EmuBenchC64.LatchA;
   }
   if (R == 0xc0e && (Value & 0x10))
   {
      EmuBenchC64.TimerA = EmuBenchC64.LatchA;
      Value &= (uint8_t)~0x10;
   }
   EmuBenchC64.Io[R] = Value;
}

CPU6502_INLINE uint8_t BasicEmuBusRead(uint16_t Addr) { return EmuBenchRead(Addr); }
CPU6502_INLINE void BasicEmuBusWrite(uint16_t Addr, uint8_t Value) { EmuBenchWrite(Addr, Value); }

CPU6502_INLINE uint32_t EmuBenchHookCalls(const uint32_t *Calls)
{
   uint32_t Sum = 0;
   for (int i = 0; i < BasicEmuHook_Count; i++) Sum += Calls[i];
   return Sum;
}

static EMU_BENCH_FN uint32_t EmuBenchChecksum(uint16_t From, uint16_t To)
{
   uint32_t Check = 0;
   for (uint32_t i = From; i <= To; i++) Check = Check * 31 + BasicEmu.Ram[i];
   return Check;
}

// TI, the jiffy clock the KERNAL's IRQ counts at $A0-$A2
CPU6502_INLINE uint32_t EmuBenchTi(void)
{
   return (uint32_t)BasicEmu.Ram[0xa0] << 16 | BasicEmu.Ram[0xa1] << 8 | BasicEmu.Ram[0xa2];
}

// Runs Prg (a .prg, load address first) on the ROMs BasicEmuInit was given. The counts in
// EmuBenchC64 and in R start at RUN.
static EMU_BENCH_FN EmuBenchStatus EmuBenchRun(const uint8_t *Prg, uint32_t PrgLen, const EmuBenchOptions *Opt,
                                               EmuBenchResult *R)
{
   memset(&EmuBenchC64, 0, sizeof EmuBenchC64);
   memset(R, 0, sizeof *R);
   EmuBenchNowNs();  // a clock keeping ClockTicks starts here; only differences are used

   // BASIC's main loop, where it asks for a line: the first time, the program goes in
   const uint16_t Main = 0xa480;
   uint8_t MainByte = BasicEmu.Basic[Main - 0xa000];
   BasicEmu.Basic[Main - 0xa000] = 0x02;

   Cpu6502 Cpu;
   memset(&Cpu, 0, sizeof Cpu);
   Cpu6502Reset(&Cpu);
   const uint64_t Slice = 64;
   double NextFrame = EMU_BENCH_PAL_FRAME;
   double RunAt = -1;
   uint64_t CyclesAtRun = 0, InstrAtRun = 0, NsAtRun = 0;
   uint32_t TiAtRun = 0;
   const uint64_t Began = EmuBenchNowNs();
   for (;;)
   {
      uint64_t Cycles = Cpu.Cycles;
      uint32_t Calls = EmuBenchHookCalls(BasicEmu.HookCalls);
      Cpu6502Stop Stop = BasicEmuRun(&Cpu, Cpu.Cycles + Slice);
      // the bus accesses advanced the clock themselves
      double Speed = RunAt < 0 ? 1 : Opt->Speed;
      double Ran = (double)(uint32_t)(Cpu.Cycles - Cycles) / Speed + (EmuBenchHookCalls(BasicEmu.HookCalls) - Calls) * Opt->HookCost;
      if (Opt->Native && (EmuBenchC64.Io[0x011] & 0x10)) Ran *= (double)EMU_BENCH_PAL_FRAME / (EMU_BENCH_PAL_FRAME - EMU_BENCH_BADLINE_STEAL);
      EmuBenchAdvance(Ran);
      if (Stop == Cpu6502_Jam && Cpu.PC == Main && RunAt < 0)
      {
         BasicEmu.Basic[Main - 0xa000] = MainByte;
         uint16_t Load = (uint16_t)(Prg[0] | Prg[1] << 8), End = (uint16_t)(Load + PrgLen - 2);
         memcpy(&BasicEmu.Ram[Load], Prg + 2, PrgLen - 2);
         BasicEmu.Ram[0x2d] = (uint8_t)End;  // VARTAB, as LOAD sets it
         BasicEmu.Ram[0x2e] = (uint8_t)(End >> 8);
         memcpy(&BasicEmu.Ram[0x277], "RUN\r", 4);
         BasicEmu.Ram[0xc6] = 4;
         RunAt = EmuBenchC64.Time;
         CyclesAtRun = Cpu.Cycles;
         InstrAtRun = Cpu.Instructions;
         TiAtRun = EmuBenchTi();
         memset(BasicEmu.HookCalls, 0, sizeof BasicEmu.HookCalls);
         memset(BasicEmu.HookNative, 0, sizeof BasicEmu.HookNative);
         EmuBenchC64.ReadCost = Opt->ReadCost;
         EmuBenchC64.WriteCost = Opt->WriteCost;
         EmuBenchC64.Reads = EmuBenchC64.Writes = EmuBenchC64.Flushed = EmuBenchC64.Frames = 0;
         memset(EmuBenchC64.ReadsBy, 0, sizeof EmuBenchC64.ReadsBy);
         memset(EmuBenchC64.WritesBy, 0, sizeof EmuBenchC64.WritesBy);
         NsAtRun = EmuBenchNowNs();
         continue;
      }
      if (Stop != Cpu6502_Budget)
      {
         R->Status = EmuBench_Stopped;
         R->StopPc = Cpu.PC;
         break;
      }
      if (!Opt->Native && EmuBenchC64.Time >= NextFrame)
      {
         NextFrame += EMU_BENCH_PAL_FRAME;
         EmuBenchC64.Frames++;
         EmuBenchC64.Flushed += BasicEmuFlushVideo();
      }
      if (RunAt >= 0 && BasicEmu.Ram[EMU_BENCH_DONE] == 1) break;
      if (EmuBenchC64.Time > Opt->MaxC64Cycles || EmuBenchNowNs() - Began > Opt->MaxNs)
      {
         R->Status = EmuBench_TimeLimit;
         R->StopPc = Cpu.PC;
         break;
      }
   }
   if (RunAt >= 0)
   {
      R->RunNs = EmuBenchNowNs() - NsAtRun;
      R->RunCycles = EmuBenchC64.Time - RunAt;
      R->Instructions = Cpu.Instructions - InstrAtRun;
      R->CpuCycles = Cpu.Cycles - CyclesAtRun;
      R->RunJiffies = EmuBenchTi() - TiAtRun;
   }
   R->Jiffies = BasicEmu.Ram[830] | BasicEmu.Ram[831] << 8;
   R->HookCalls = EmuBenchHookCalls(BasicEmu.HookCalls);
   R->HookNative = EmuBenchHookCalls(BasicEmu.HookNative);
   R->Bitmap = EmuBenchChecksum(0x2000, 0x3f3f);
   R->Screen = EmuBenchChecksum(0x0400, 0x07e7);
   BasicEmu.Basic[Main - 0xa000] = MainByte;
   return R->Status;
}

#endif
