// SPDX-License-Identifier: MIT
// cbm_float_harness: reads "ROUTINE A X Y P ZP [BYTES]" lines from stdin. ROUTINE is one of the
// names in Routines; A to P are two hex digits each and ZP the whole zero page as 512 hex digits.
// BYTES, in hex, is the five bytes at A/Y for fadd, fsub, fmult and fdiv, and the text from
// TXTPTR on for fin. Runs that routine's port on each and prints "RESULT SET A X Y P ZP USED" in
// the same format, RESULT being "done", "native" or "more", SET the 4-digit hex mask of what the
// call determined, and USED how many text bytes fin read. Each result is flushed as it is
// printed, so a script can drive it a line at a time.
#include <stdio.h>
#include <string.h>

#include "../BasicAccel/CbmFloat.h"

enum Kind { ZeroPage, Operand, Text };

static const struct {
   const char *Name;
   enum Kind Kind;
   CbmFloatResult (*Run)(CbmCpu *);
   CbmFloatResult (*RunOperand)(CbmCpu *, const uint8_t[5]);
} Routines[] = {
   { "faddt", ZeroPage, CbmFAddT, NULL }, { "fsubt", ZeroPage, CbmFSubT, NULL },
   { "fmultt", ZeroPage, CbmFMultT, NULL }, { "fdivt", ZeroPage, CbmFDivT, NULL },
   { "fadd", Operand, NULL, CbmFAdd }, { "fsub", Operand, NULL, CbmFSub },
   { "fmult", Operand, NULL, CbmFMult }, { "fdiv", Operand, NULL, CbmFDiv },
   { "fin", Text, NULL, NULL },
   { "qint", ZeroPage, CbmQint, NULL }, { "ayint", ZeroPage, CbmAyint, NULL },
};

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
   static char Line[4096];
   static uint8_t Bytes[1024];
   while (fgets(Line, sizeof Line, stdin))
   {
      int R = -1;
      const char *Args = strchr(Line, ' ');
      for (size_t i = 0; Args && i < sizeof Routines / sizeof Routines[0]; i++)
         if ((size_t)(Args - Line) == strlen(Routines[i].Name) && strncmp(Line, Routines[i].Name, Args - Line) == 0)
            R = (int)i;
      CbmCpu Cpu;
      uint8_t Regs[4];
      if (R < 0 || strlen(++Args) < 4 * 3 + 512 || !Hex(Args, &Regs[0], 1) || !Hex(Args + 3, &Regs[1], 1) ||
          !Hex(Args + 6, &Regs[2], 1) || !Hex(Args + 9, &Regs[3], 1) || !Hex(Args + 12, Cpu.Zp, 256))
      {
         fprintf(stderr, "bad line: %s", Line);
         return 2;
      }
      Cpu.A = Regs[0]; Cpu.X = Regs[1]; Cpu.Y = Regs[2]; Cpu.P = Regs[3];
      size_t Len = 0;
      if (Routines[R].Kind != ZeroPage)
      {
         const char *Hexes = Args + 12 + 512 + 1;
         Len = strcspn(Hexes, " \r\n") / 2;
         if (Hexes[-1] != ' ' || Len > sizeof Bytes || !Hex(Hexes, Bytes, Len) || (Routines[R].Kind == Operand && Len != 5))
         {
            fprintf(stderr, "bad bytes: %s", Line);
            return 2;
         }
      }
      CbmText Text = { Bytes, (unsigned)Len, 0 };
      CbmFloatResult Result;
      switch (Routines[R].Kind)
      {
         case ZeroPage: Result = Routines[R].Run(&Cpu); break;
         case Operand: Result = Routines[R].RunOperand(&Cpu, Bytes); break;
         default: Result = CbmFin(&Cpu, &Text); break;
      }
      printf("%s %04x %02x %02x %02x %02x ", Result == CbmFloat_Done ? "done" : Result == CbmFloat_Native ? "native" : "more",
             Cpu.Set, Cpu.A, Cpu.X, Cpu.Y, Cpu.P);
      PrintHex(Cpu.Zp, 256);
      printf(" %u\n", Text.Used);
      fflush(stdout);
   }
   return 0;
}
