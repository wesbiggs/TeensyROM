// SPDX-License-Identifier: MIT
// emu_model: Source/Teensy/BasicAccel/EmuBench.h on the host, which says what the run and the
// model are.
//
//   emu_model BASIC KERNAL CHARGEN PROGRAM.prg [--speed N] [--read N] [--write N] [--hook N]
//             [--hooks MASK] [--native] [--dump FILE]
//
// Prints what the run took as JSON. --dump FILE writes the 64K of RAM at the end.
#define _POSIX_C_SOURCE 200809L  // clock_gettime under -std=c99
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../Source/Teensy/BasicAccel/EmuBench.h"

static inline uint64_t EmuBenchNowNs(void)
{
   struct timespec T;
   clock_gettime(CLOCK_MONOTONIC, &T);
   return (uint64_t)T.tv_sec * 1000000000u + (uint64_t)T.tv_nsec;
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

int main(int argc, char **argv)
{
   EmuBenchOptions Opt = { 20, 3, 1, 2, false, 600e6, UINT64_MAX };
   uint32_t Hooks = (1u << BasicEmuHook_Count) - 1;
   const char *Dump = NULL;
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
      else if (i + 1 < argc && !strcmp(argv[i], "--hooks")) Hooks = (uint32_t)strtoul(argv[++i], NULL, 0);
      else if (i + 1 < argc && !strcmp(argv[i], "--dump")) Dump = argv[++i];
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
      Hooks = 0;
   }
   uint8_t *Basic = ReadFile(argv[1], 0x2000, NULL), *Kernal = ReadFile(argv[2], 0x2000, NULL);
   uint8_t *Chargen = ReadFile(argv[3], 0x1000, NULL);
   size_t PrgLen;
   uint8_t *Prg = ReadFile(argv[4], 0, &PrgLen);
   if (PrgLen < 3 || (Prg[0] | Prg[1] << 8) + PrgLen - 2 > 0xa000)
   {
      fprintf(stderr, "%s does not fit below $A000\n", argv[4]);
      return 2;
   }
   BasicEmuInit(NULL, Basic, Kernal, Chargen, Hooks);

   EmuBenchResult R;
   clock_t Began = clock();
   EmuBenchStatus Status = EmuBenchRun(Prg, (uint32_t)PrgLen, &Opt, &R);
   double Host = (double)(clock() - Began) / CLOCKS_PER_SEC;
   if (Status == EmuBench_Stopped)
   {
      fprintf(stderr, "stopped at $%04x on $%02x\n", R.StopPc, Cpu6502Read(R.StopPc));
      return 1;
   }
   if (Status == EmuBench_TimeLimit)
   {
      fprintf(stderr, "no result after 10 minutes of C64 time, at $%04x\n", R.StopPc);
      return 1;
   }
   if (Dump)
   {
      FILE *F = fopen(Dump, "wb");
      if (!F || fwrite(BasicEmu.Ram, 1, 0x10000, F) != 0x10000 || fclose(F))
      {
         fprintf(stderr, "cannot write %s\n", Dump);
         return 2;
      }
   }
   unsigned DirtyLeft = 0;
   for (unsigned i = 0; i < sizeof BasicEmu.Dirty; i++) DirtyLeft += (unsigned)__builtin_popcount(BasicEmu.Dirty[i]);

   printf("{\"speed\":%g,\"read\":%g,\"write\":%g,\"hook\":%g,\"native\":%s,\"hooks\":%u,", Opt.Speed, Opt.ReadCost,
          Opt.WriteCost, Opt.HookCost, Opt.Native ? "true" : "false", Hooks);
   printf("\"runCycles\":%.0f,\"runJiffies\":%u,\"jiffies\":%u,", R.RunCycles, R.RunJiffies, R.Jiffies);
   printf("\"instructions\":%llu,\"cpuCycles\":%llu,", (unsigned long long)R.Instructions, (unsigned long long)R.CpuCycles);
   printf("\"busReads\":%llu,\"busWrites\":%llu,\"flushed\":%llu,\"frames\":%llu,\"dirtyLeft\":%u,",
          (unsigned long long)EmuBenchC64.Reads, (unsigned long long)EmuBenchC64.Writes,
          (unsigned long long)EmuBenchC64.Flushed, (unsigned long long)EmuBenchC64.Frames, DirtyLeft);
   printf("\"readsBy\":{");
   for (int i = 0, n = 0; i < 16; i++)
      if (EmuBenchC64.ReadsBy[i]) printf("%s\"d%x\":%llu", n++ ? "," : "", i, (unsigned long long)EmuBenchC64.ReadsBy[i]);
   printf("},\"writesBy\":{");
   for (int i = 0, n = 0; i < 16; i++)
      if (EmuBenchC64.WritesBy[i]) printf("%s\"d%x\":%llu", n++ ? "," : "", i, (unsigned long long)EmuBenchC64.WritesBy[i]);
   printf("},\"hookCalls\":[");
   for (int i = 0; i < BasicEmuHook_Count; i++) printf("%s%u", i ? "," : "", BasicEmu.HookCalls[i]);
   printf("],\"hookNative\":[");
   for (int i = 0; i < BasicEmuHook_Count; i++) printf("%s%u", i ? "," : "", BasicEmu.HookNative[i]);
   printf("],\"bitmap\":%u,\"screen\":%u,\"hostSeconds\":%.3f,\"runNs\":%llu}\n", R.Bitmap, R.Screen, Host,
          (unsigned long long)R.RunNs);
   return 0;
}
