// SPDX-License-Identifier: MIT
// cbm_fmult_harness: reads "A X Y P ZP" lines from stdin, A to P two hex digits each and ZP the
// whole zero page as 512 hex digits, runs CbmFMultT on each and prints "RESULT SET A X Y P ZP"
// in the same format, RESULT being "done" or "native" and SET the 4-digit hex mask of what the
// call determined. Each result is flushed as it is printed, so a script can drive it a line at
// a time.
#include <stdio.h>
#include <string.h>

#include "../BasicAccel/CbmFMult.h"

static int Hex(const char *s, uint8_t *Out, size_t Count)
{
   for (size_t i = 0; i < Count; i++)
   {
      unsigned v;
      if (sscanf(s + 2 * i, "%2x", &v) != 1) return 0;
      Out[i] = (uint8_t)v;
   }
   return 1;
}

static void PrintHex(const uint8_t *Data, size_t Count)
{
   for (size_t i = 0; i < Count; i++) printf("%02x", Data[i]);
}

int main(void)
{
   static char Line[1024];
   while (fgets(Line, sizeof Line, stdin))
   {
      CbmCpu Cpu;
      uint8_t Regs[4];
      if (strlen(Line) < 4 * 3 + 512 || !Hex(Line, &Regs[0], 1) || !Hex(Line + 3, &Regs[1], 1) ||
          !Hex(Line + 6, &Regs[2], 1) || !Hex(Line + 9, &Regs[3], 1) || !Hex(Line + 12, Cpu.Zp, 256))
      {
         fprintf(stderr, "bad line: %s", Line);
         return 2;
      }
      Cpu.A = Regs[0]; Cpu.X = Regs[1]; Cpu.Y = Regs[2]; Cpu.P = Regs[3];
      CbmFMultResult Result = CbmFMultT(&Cpu);
      printf("%s %04x %02x %02x %02x %02x ", Result == CbmFMult_Done ? "done" : "native", Cpu.Set, Cpu.A, Cpu.X, Cpu.Y, Cpu.P);
      PrintHex(Cpu.Zp, 256);
      printf("\n");
      fflush(stdout);
   }
   return 0;
}
