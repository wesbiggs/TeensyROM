// SPDX-License-Identifier: MIT
// emu_model: BasicEmu.h on the host, with a model of the C64 on the other side of the bus.
//
//   emu_model BASIC KERNAL CHARGEN PROGRAM.prg [options]
//
// Boots the ROMs, types RUN for PROGRAM at BASIC's first prompt, and runs until the program
// writes 1 to $033C (SinePlot.bas does when it has drawn). Prints what it took as JSON.
//
// The model's clock is the C64's, in its cycles. From RUN, the 6502 here runs --speed native
// cycles per C64 cycle; each bus read stalls it --read cycles and each bus write (an I/O write, or a dirty
// byte flushed to the VIC once a frame) costs --write. --native models the C64's own CPU
// instead: speed 1, the bus free, no flush, and the cycles the VIC takes on badlines. --hook is
// what one answered float routine costs, in C64 cycles. The counts in the output start at RUN.
// --dump FILE writes the 64K of RAM at the end.
//
// The C64's side is what the KERNAL and BASIC need: CIA1's timer A and its IRQ (the jiffy
// clock), the VIC's raster counter and raster latch (the KERNAL's PAL/NTSC check), and every
// other register holding what was written. Nothing is drawn; the RAM is the result.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../Source/Teensy/BasicAccel/BasicEmu.h"

#define PAL_LINES 312
#define PAL_LINE_CYCLES 63
#define PAL_FRAME (PAL_LINES * PAL_LINE_CYCLES)
#define BADLINE_STEAL (25 * 40)  // cycles a frame the VIC takes for character pointers

static struct {
   double Speed, ReadCost, WriteCost, HookCost;
   bool Native;
   uint32_t Hooks;
   const char *Dump;
} Opt = { 20, 3, 1, 2, false, (1u << BasicEmuHook_Count) - 1, NULL };

static struct {
   double Time;  // C64 cycles since reset
   uint64_t CiaCycles;  // whole cycles of Time the timer has counted
   uint8_t Io[0x1000];
   int32_t TimerA;
   uint16_t LatchA;
   uint8_t Icr, IcrMask;
   uint8_t VicIrq;
   uint32_t LastLine;  // raster lines since reset, at the last update
} C64;

static struct {
   uint64_t Reads, Writes, Flushed, Frames;
   uint64_t ReadsBy[16], WritesBy[16];  // by I/O page, $D0 to $DF
} Bus;

// The VIC's raster, and its latch for the compare line
static void VicUpdate(void)
{
   uint32_t Lines = (uint32_t)(C64.Time / PAL_LINE_CYCLES);
   uint32_t Compare = C64.Io[0x012] | (C64.Io[0x011] & 0x80) << 1;
   if (Compare < PAL_LINES && Lines != C64.LastLine)
   {
      uint32_t Crossed = Lines - C64.LastLine;
      uint32_t From = (C64.LastLine + 1) % PAL_LINES;
      if (Crossed >= PAL_LINES || (Compare - From + PAL_LINES) % PAL_LINES < Crossed) C64.VicIrq |= 1;
   }
   C64.LastLine = Lines;
}

// The timer counts whole cycles of the clock, so the fractions the slices leave add up
static void CiaUpdate(void)
{
   uint64_t Now = (uint64_t)C64.Time, Elapsed = Now - C64.CiaCycles;
   C64.CiaCycles = Now;
   if (!(C64.Io[0xc0e] & 1)) return;
   C64.TimerA -= (int32_t)Elapsed;
   while (C64.TimerA < 0)
   {
      C64.Icr |= 1;
      if (C64.Io[0xc0e] & 8)
      {
         C64.Io[0xc0e] &= ~1;
         C64.TimerA = C64.LatchA;
         break;
      }
      C64.TimerA += C64.LatchA + 1;
   }
}

static void Advance(double Cycles)
{
   C64.Time += Cycles;
   CiaUpdate();
   VicUpdate();
}

static inline bool Cpu6502IrqLine(void) { return (C64.Icr & C64.IcrMask & 0x1f) || (C64.VicIrq & C64.Io[0x01a] & 0x0f); }
static inline bool Cpu6502TakeNmi(void) { return false; }

static inline uint8_t BasicEmuBusRead(uint16_t Addr)
{
   Bus.Reads++;
   Bus.ReadsBy[(Addr >> 8) & 15]++;
   Advance(Opt.ReadCost);
   uint16_t R = Addr & 0xfff;
   if (R < 0x400)
   {
      R &= 0x3f;
      if (R == 0x11) return (uint8_t)((C64.Io[0x011] & 0x7f) | ((C64.LastLine % PAL_LINES) >> 1 & 0x80));
      if (R == 0x12) return (uint8_t)(C64.LastLine % PAL_LINES);
      if (R == 0x19) return (uint8_t)(C64.VicIrq | ((C64.VicIrq & C64.Io[0x01a] & 0x0f) ? 0x80 : 0) | 0x70);
      return C64.Io[R];
   }
   if (R >= 0x800 && R < 0xc00) return C64.Io[R] | 0xf0;  // color RAM is four bits
   if (R >= 0xc00 && R < 0xd00)
   {
      R = 0xc00 | (R & 0x0f);
      if (R == 0xc00 || R == 0xc01) return 0xff;  // no key down, no joystick
      if (R == 0xc04) return (uint8_t)C64.TimerA;
      if (R == 0xc05) return (uint8_t)(C64.TimerA >> 8);
      if (R == 0xc0d)
      {
         uint8_t Value = (uint8_t)(C64.Icr | ((C64.Icr & C64.IcrMask & 0x1f) ? 0x80 : 0));
         C64.Icr = 0;
         return Value;
      }
   }
   return C64.Io[R];
}

static inline void BasicEmuBusWrite(uint16_t Addr, uint8_t Value)
{
   Bus.Writes++;
   if (Addr >= 0xd000 && Addr < 0xe000) Bus.WritesBy[(Addr >> 8) & 15]++;
   Advance(Opt.WriteCost);
   if (Addr < 0xd000 || Addr >= 0xe000) return;  // a flushed RAM byte: the RAM is BasicEmu's
   uint16_t R = Addr & 0xfff;
   if (R < 0x400) R &= 0x3f;
   if (R >= 0xc00 && R < 0xd00) R = 0xc00 | (R & 0x0f);
   if (R == 0x019)
   {
      C64.VicIrq &= (uint8_t)~(Value & 0x0f);
      return;
   }
   if (R == 0xc0d)
   {
      if (Value & 0x80) C64.IcrMask |= Value & 0x1f;
      else C64.IcrMask &= (uint8_t)~(Value & 0x1f);
      return;
   }
   if (R == 0xc04) C64.LatchA = (C64.LatchA & 0xff00) | Value;
   if (R == 0xc05)
   {
      C64.LatchA = (uint16_t)((C64.LatchA & 0xff) | Value << 8);
      if (!(C64.Io[0xc0e] & 1)) C64.TimerA = C64.LatchA;
   }
   if (R == 0xc0e && (Value & 0x10))
   {
      C64.TimerA = C64.LatchA;
      Value &= (uint8_t)~0x10;
   }
   C64.Io[R] = Value;
}

static uint8_t *ReadFile(const char *Path, size_t Want, size_t *Got)
{
   FILE *F = fopen(Path, "rb");
   if (!F)
   {
      fprintf(stderr, "cannot open %s\n", Path);
      exit(2);
   }
   uint8_t *Data = calloc(1, 0x10000);
   size_t Len = fread(Data, 1, 0x10000, F);
   fclose(F);
   if (Want && Len != Want)
   {
      fprintf(stderr, "%s: %zu bytes, not %zu\n", Path, Len, Want);
      exit(2);
   }
   if (Got) *Got = Len;
   return Data;
}

static uint32_t HookCalls(void)
{
   uint32_t Calls = 0;
   for (int i = 0; i < BasicEmuHook_Count; i++) Calls += BasicEmu.HookCalls[i];
   return Calls;
}

static uint32_t Checksum(uint16_t From, uint16_t To)
{
   uint32_t Check = 0;
   for (uint32_t i = From; i <= To; i++) Check = Check * 31 + BasicEmu.Ram[i];
   return Check;
}

// TI, the jiffy clock the KERNAL's IRQ counts at $A0-$A2
static uint32_t Ti(void) { return (uint32_t)BasicEmu.Ram[0xa0] << 16 | BasicEmu.Ram[0xa1] << 8 | BasicEmu.Ram[0xa2]; }

int main(int argc, char **argv)
{
   if (argc < 5)
   {
      fprintf(stderr, "usage: emu_model BASIC KERNAL CHARGEN PROGRAM.prg [--speed N] [--read N] [--write N] [--hook N]\n"
                      "                 [--hooks MASK] [--native] [--dump FILE]\n");
      return 2;
   }
   for (int i = 5; i < argc; i++)
   {
      if (!strcmp(argv[i], "--native")) Opt.Native = true;
      else if (i + 1 < argc && !strcmp(argv[i], "--speed")) Opt.Speed = atof(argv[++i]);
      else if (i + 1 < argc && !strcmp(argv[i], "--read")) Opt.ReadCost = atof(argv[++i]);
      else if (i + 1 < argc && !strcmp(argv[i], "--write")) Opt.WriteCost = atof(argv[++i]);
      else if (i + 1 < argc && !strcmp(argv[i], "--hook")) Opt.HookCost = atof(argv[++i]);
      else if (i + 1 < argc && !strcmp(argv[i], "--hooks")) Opt.Hooks = (uint32_t)strtoul(argv[++i], NULL, 0);
      else if (i + 1 < argc && !strcmp(argv[i], "--dump")) Opt.Dump = argv[++i];
      else
      {
         fprintf(stderr, "unknown option %s\n", argv[i]);
         return 2;
      }
   }
   if (Opt.Native)
   {
      Opt.Speed = 1;
      Opt.ReadCost = Opt.WriteCost = 0;
      Opt.Hooks = 0;
   }
   uint8_t *Basic = ReadFile(argv[1], 0x2000, NULL), *Kernal = ReadFile(argv[2], 0x2000, NULL);
   uint8_t *Chargen = ReadFile(argv[3], 0x1000, NULL);
   size_t PrgLen;
   uint8_t *Prg = ReadFile(argv[4], 0, &PrgLen);
   static uint8_t Ram[0x10000];
   BasicEmuInit(Ram, Basic, Kernal, Chargen, Opt.Hooks);

   // BASIC's main loop, where it asks for a line: the first time, the program goes in
   const uint16_t Main = 0xa480;
   BasicEmu.Basic[Main - 0xa000] = 0x02;

   Cpu6502 Cpu = { 0 };
   Cpu6502Reset(&Cpu);
   const uint64_t Slice = 64;
   uint64_t NextFrame = PAL_FRAME;
   double RunAt = -1;
   uint64_t CyclesAtRun = 0, InstrAtRun = 0;
   uint32_t TiAtRun = 0;
   clock_t Began = clock();
   for (;;)
   {
      uint64_t Cycles = Cpu.Cycles;
      uint32_t Calls = HookCalls();
      Cpu6502Stop Stop = BasicEmuRun(&Cpu, Cpu.Cycles + Slice);
      // the bus accesses advanced the clock themselves
      // The boot runs at the C64's speed: faster, the KERNAL clears the screen in under a frame,
      // its PAL check ($FF5E) finds the raster latch for line 311 unset, and TI runs NTSC-slow
      double Speed = RunAt < 0 ? 1 : Opt.Speed;
      double Ran = (double)(Cpu.Cycles - Cycles) / Speed + (HookCalls() - Calls) * Opt.HookCost;
      if (Opt.Native && (C64.Io[0x011] & 0x10)) Ran *= (double)PAL_FRAME / (PAL_FRAME - BADLINE_STEAL);
      Advance(Ran);
      if (Stop == Cpu6502_Jam && Cpu.PC == Main && RunAt < 0)
      {
         BasicEmu.Basic[Main - 0xa000] = Basic[Main - 0xa000];
         uint16_t Load = (uint16_t)(Prg[0] | Prg[1] << 8), End = (uint16_t)(Load + PrgLen - 2);
         memcpy(&BasicEmu.Ram[Load], Prg + 2, PrgLen - 2);
         BasicEmu.Ram[0x2d] = (uint8_t)End;  // VARTAB, as LOAD sets it
         BasicEmu.Ram[0x2e] = (uint8_t)(End >> 8);
         memcpy(&BasicEmu.Ram[0x277], "RUN\r", 4);
         BasicEmu.Ram[0xc6] = 4;
         RunAt = C64.Time;
         CyclesAtRun = Cpu.Cycles;
         InstrAtRun = Cpu.Instructions;
         TiAtRun = Ti();
         memset(&Bus, 0, sizeof Bus);
         continue;
      }
      if (Stop != Cpu6502_Budget)
      {
         fprintf(stderr, "stopped at $%04x on $%02x\n", Cpu.PC, Cpu6502Read(Cpu.PC));
         return 1;
      }
      if (!Opt.Native && C64.Time >= NextFrame)
      {
         NextFrame += PAL_FRAME;
         Bus.Frames++;
         Bus.Flushed += BasicEmuFlushVideo();
      }
      if (RunAt >= 0 && BasicEmu.Ram[0x33c] == 1) break;
      if (C64.Time > 600e6)
      {
         fprintf(stderr, "no result after 10 minutes of C64 time, at $%04x\n", Cpu.PC);
         return 1;
      }
   }
   double Host = (double)(clock() - Began) / CLOCKS_PER_SEC;
   if (Opt.Dump)
   {
      FILE *F = fopen(Opt.Dump, "wb");
      if (!F || fwrite(BasicEmu.Ram, 1, 0x10000, F) != 0x10000 || fclose(F))
      {
         fprintf(stderr, "cannot write %s\n", Opt.Dump);
         return 2;
      }
   }
   unsigned DirtyLeft = 0;
   for (unsigned i = 0; i < sizeof BasicEmu.Dirty; i++) DirtyLeft += (unsigned)__builtin_popcount(BasicEmu.Dirty[i]);

   printf("{\"speed\":%g,\"read\":%g,\"write\":%g,\"hook\":%g,\"native\":%s,\"hooks\":%u,", Opt.Speed, Opt.ReadCost,
          Opt.WriteCost, Opt.HookCost, Opt.Native ? "true" : "false", Opt.Hooks);
   printf("\"runCycles\":%.0f,\"runJiffies\":%u,\"jiffies\":%u,", C64.Time - RunAt, Ti() - TiAtRun,
          BasicEmu.Ram[830] | BasicEmu.Ram[831] << 8);
   printf("\"instructions\":%llu,\"cpuCycles\":%llu,", (unsigned long long)(Cpu.Instructions - InstrAtRun),
          (unsigned long long)(Cpu.Cycles - CyclesAtRun));
   printf("\"busReads\":%llu,\"busWrites\":%llu,\"flushed\":%llu,\"frames\":%llu,\"dirtyLeft\":%u,",
          (unsigned long long)Bus.Reads, (unsigned long long)Bus.Writes, (unsigned long long)Bus.Flushed,
          (unsigned long long)Bus.Frames, DirtyLeft);
   printf("\"readsBy\":{");
   for (int i = 0, n = 0; i < 16; i++)
      if (Bus.ReadsBy[i]) printf("%s\"d%x\":%llu", n++ ? "," : "", i, (unsigned long long)Bus.ReadsBy[i]);
   printf("},\"writesBy\":{");
   for (int i = 0, n = 0; i < 16; i++)
      if (Bus.WritesBy[i]) printf("%s\"d%x\":%llu", n++ ? "," : "", i, (unsigned long long)Bus.WritesBy[i]);
   printf("},\"hookCalls\":[");
   for (int i = 0; i < BasicEmuHook_Count; i++) printf("%s%u", i ? "," : "", BasicEmu.HookCalls[i]);
   printf("],\"hookNative\":[");
   for (int i = 0; i < BasicEmuHook_Count; i++) printf("%s%u", i ? "," : "", BasicEmu.HookNative[i]);
   printf("],\"bitmap\":%u,\"screen\":%u,\"hostSeconds\":%.3f}\n", Checksum(0x2000, 0x3f3f), Checksum(0x0400, 0x07e7), Host);
   return 0;
}
