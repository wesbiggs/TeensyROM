// SPDX-License-Identifier: MIT
// cpu6502_harness: reads "SEED PC A X Y S P IRQ NMI [ADDR:VALUE]..." lines (hex) from stdin.
// Memory is VALUE at each ADDR given and Cpu6502TestByte(SEED, address) everywhere else. Runs
// one instruction, after taking the interrupt IRQ or NMI asks for, and prints
// "STOP PC A X Y S P CYCLES" then each write as " ADDR:VALUE", in order, a read-modify-write's
// two writes included.
#include <stdio.h>
#include <string.h>

#include "../BasicAccel/Cpu6502.h"

static uint32_t Seed;
static uint8_t Mem[0x10000];
static uint8_t Written[0x10000];
static bool Irq, Nmi;
static char Writes[1 << 14];
static size_t WritesLen;

uint8_t Cpu6502TestByte(uint32_t Seed, uint16_t Addr)
{
   uint32_t H = (Addr * 0x9e3779b1u) ^ Seed;
   H ^= H >> 15;
   H *= 0x85ebca77u;
   H ^= H >> 13;
   return (uint8_t)H;
}

static inline uint8_t Cpu6502Read(uint16_t Addr) { return Written[Addr] ? Mem[Addr] : Cpu6502TestByte(Seed, Addr); }

static inline void Cpu6502Write(uint16_t Addr, uint8_t Value)
{
   Mem[Addr] = Value;
   Written[Addr] = 1;
   if (WritesLen + 9 < sizeof Writes) WritesLen += (size_t)snprintf(Writes + WritesLen, 9, " %04x:%02x", Addr, Value);
}

static inline void Cpu6502Modify(uint16_t Addr, uint8_t Old, uint8_t New)
{
   Cpu6502Write(Addr, Old);
   Cpu6502Write(Addr, New);
}

static inline bool Cpu6502IrqLine(void) { return Irq; }

static inline bool Cpu6502TakeNmi(void)
{
   bool Was = Nmi;
   Nmi = false;
   return Was;
}

int main(void)
{
   static char Line[4096];
   while (fgets(Line, sizeof Line, stdin))
   {
      unsigned S, Pc, A, X, Y, Sp, P, I, N;
      int Used;
      if (sscanf(Line, "%x %x %x %x %x %x %x %x %x%n", &S, &Pc, &A, &X, &Y, &Sp, &P, &I, &N, &Used) != 9)
      {
         fprintf(stderr, "bad line: %s", Line);
         return 2;
      }
      Seed = S;
      memset(Written, 0, sizeof Written);
      for (const char *At = Line + Used; *At == ' ';)
      {
         unsigned Addr, Value;
         int Len;
         if (sscanf(At, " %x:%x%n", &Addr, &Value, &Len) != 2 || Addr > 0xffff || Value > 0xff)
         {
            fprintf(stderr, "bad override: %s", Line);
            return 2;
         }
         Mem[Addr] = (uint8_t)Value;
         Written[Addr] = 1;
         At += Len;
      }
      Irq = I;
      Nmi = N;
      Cpu6502 Cpu = { (uint16_t)Pc, (uint8_t)A, (uint8_t)X, (uint8_t)Y, (uint8_t)Sp, (uint8_t)(P | CPU6502_U), 0, 0 };
      WritesLen = 0;
      Writes[0] = 0;
      Cpu6502Stop Stop = Cpu6502Run(&Cpu, 1);
      printf("%s %04x %02x %02x %02x %02x %02x %llu%s\n",
             Stop == Cpu6502_Budget ? "ran" : Stop == Cpu6502_Jam ? "jam" : "undefined", Cpu.PC, Cpu.A, Cpu.X, Cpu.Y,
             Cpu.S, Cpu.P, (unsigned long long)Cpu.Cycles, Writes);
      fflush(stdout);
   }
   return 0;
}
