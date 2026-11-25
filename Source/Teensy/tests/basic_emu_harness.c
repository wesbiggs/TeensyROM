// SPDX-License-Identifier: MIT
// basic_emu_harness: BasicEmu.h on made-up ROMs, so it runs without the C64's. Each check
// prints "ok NAME" or "FAIL NAME: what differed"; the exit status is the number that failed.
#include <stdio.h>
#include <string.h>

#include "../BasicAccel/BasicEmu.h"

static struct {
   unsigned Reads, Writes;
   uint16_t Addr[64];
   uint8_t Value[64];
} Bus;

static inline uint8_t BasicEmuBusRead(uint16_t Addr)
{
   (void)Addr;
   Bus.Reads++;
   return 0xb5;
}

static inline void BasicEmuBusWrite(uint16_t Addr, uint8_t Value)
{
   if (Bus.Writes < 64)
   {
      Bus.Addr[Bus.Writes] = Addr;
      Bus.Value[Bus.Writes] = Value;
   }
   Bus.Writes++;
}

static inline bool Cpu6502IrqLine(void) { return false; }
static inline bool Cpu6502TakeNmi(void) { return false; }

static int Failed;

static void Check(const char *Name, bool Ok, const char *Detail)
{
   if (Ok) printf("ok %s\n", Name);
   else
   {
      printf("FAIL %s: %s\n", Name, Detail);
      Failed++;
   }
}

static uint8_t Ram[0x10000], Basic[0x2000], Kernal[0x2000], Chargen[0x1000];

static void Setup(uint32_t Hooks)
{
   memset(Ram, 0, sizeof Ram);
   memset(Basic, 0x60, sizeof Basic);  // RTS everywhere
   memset(Kernal, 0x33, sizeof Kernal);
   memset(Chargen, 0x55, sizeof Chargen);
   Basic[0] = 0x22;
   Ram[0xa000] = 0x11;
   Ram[0xe000] = 0x44;
   Ram[0xd000] = 0x66;
   BasicEmuInit(Ram, Basic, Kernal, Chargen, Hooks);
   memset(&Bus, 0, sizeof Bus);
}

// Runs the code at $1000 to the JAM at its end
static Cpu6502 Call(const uint8_t *Code, size_t Len)
{
   memcpy(&BasicEmu.Ram[0x1000], Code, Len);
   BasicEmu.Ram[0x1000 + Len] = 0x12;  // JAM, not a hook
   Cpu6502 Cpu = { 0x1000, 0, 0, 0, 0xff, CPU6502_U | CPU6502_I, 0, 0 };
   BasicEmuRun(&Cpu, 1000000);
   return Cpu;
}

static void Banking(void)
{
   // $01's low three bits, and what $A000, $D000 and $E000 read as: R RAM, B BASIC, K KERNAL,
   // C character ROM, I the bus
   static const struct { uint8_t Port; const char *Map; } Modes[] = {
      { 0x37, "BIK" }, { 0x36, "RIK" }, { 0x35, "RIR" }, { 0x34, "RRR" }, { 0x33, "BCK" }, { 0x32, "RCK" }, { 0x31, "RCR" },
   };
   for (size_t i = 0; i < sizeof Modes / sizeof Modes[0]; i++)
   {
      Setup(0);
      Cpu6502Write(1, Modes[i].Port);
      uint8_t A = Cpu6502Read(0xa000), D = Cpu6502Read(0xd000), E = Cpu6502Read(0xe000);
      char Got[4] = { A == 0x22 ? 'B' : A == 0x11 ? 'R' : '?', D == 0x55 ? 'C' : D == 0x66 ? 'R' : D == 0xb5 ? 'I' : '?',
                      E == 0x33 ? 'K' : E == 0x44 ? 'R' : '?', 0 };
      char Name[32], Detail[64];
      snprintf(Name, sizeof Name, "banking $%02x", Modes[i].Port);
      snprintf(Detail, sizeof Detail, "read %s, not %s", Got, Modes[i].Map);
      Check(Name, strcmp(Got, Modes[i].Map) == 0, Detail);
   }

   Setup(0);
   Cpu6502Write(0xa000, 0x99);
   Cpu6502Write(0xe000, 0x98);
   Cpu6502Write(0xd020, 0x97);
   Check("writes under ROM go to RAM, and I/O to the bus",
         BasicEmu.Ram[0xa000] == 0x99 && BasicEmu.Ram[0xe000] == 0x98 && BasicEmu.Ram[0xd020] == 0 && Bus.Writes == 1 &&
            Bus.Addr[0] == 0xd020 && Bus.Value[0] == 0x97,
         "a write landed in the wrong place");
   Cpu6502Write(1, 0x34);
   Cpu6502Write(0xd020, 0x96);
   Check("with I/O banked out, $D000-$DFFF is RAM", BasicEmu.Ram[0xd020] == 0x96 && Bus.Writes == 1, "it went to the bus");
   Check("$01 reads its outputs and pulled-up inputs", Cpu6502Read(1) == 0x34 && Cpu6502Read(0) == 0x2f, "wrong port value");
}

static void ModifyOnIo(void)
{
   Setup(0);
   static const uint8_t Code[] = { 0xee, 0x19, 0xd0, 0xee, 0x00, 0x30 };  // INC $D019, INC $3000
   Call(Code, sizeof Code);
   Check("INC on I/O writes the old value, then the new",
         Bus.Reads == 1 && Bus.Writes == 2 && Bus.Value[0] == 0xb5 && Bus.Value[1] == 0xb6 && BasicEmu.Ram[0x3000] == 1,
         "wrong bus writes");
}

static void Hooks(void)
{
   // FAC = 3, operand at $2000 = 2: FMULT gives 6, and returns to the caller
   static const uint8_t Code[] = { 0xa9, 0x00, 0xa0, 0x20, 0x20, 0x28, 0xba };  // LDA #0, LDY #$20, JSR FMULT
   static const uint8_t Three[] = { 0x82, 0xc0, 0, 0, 0 }, Two[] = { 0x82, 0, 0, 0, 0 };  // FAC unpacked, the operand packed
   Setup(1u << BasicEmuHook_FMult);
   memcpy(&BasicEmu.Ram[0x61], Three, 5);
   BasicEmu.Ram[0x66] = 0;
   memcpy(&BasicEmu.Ram[0x2000], Two, 5);

   static CbmCpu Want;
   memcpy(Want.Zp, BasicEmu.Ram, 256);
   Want.A = 0;
   Want.Y = 0x20;
   Want.X = 0;
   Want.P = CPU6502_U | CPU6502_I;  // LDY #$20 leaves N and Z clear
   Want.Set = 0;
   CbmFMult(&Want, Two);

   Cpu6502 Cpu = Call(Code, sizeof Code);
   Check("a hook answers FMULT and returns to the caller",
         Cpu.PC == 0x1000 + sizeof Code && Cpu.S == 0xff && BasicEmu.HookCalls[BasicEmuHook_FMult] == 1 &&
            BasicEmu.HookNative[BasicEmuHook_FMult] == 0,
         "did not return from the hook");
   Check("the hook's zero page and registers are CbmFMult's",
         memcmp(&BasicEmu.Ram[2], &Want.Zp[2], 254) == 0 && BasicEmu.Ram[0x61] == 0x83 && BasicEmu.Ram[0x62] == 0xc0 &&
            (!(Want.Set & CBM_SET_A) || Cpu.A == Want.A),
         "the result differs");
   Check("the hook marks what it changed dirty", BasicEmu.Dirty[0x61 >> 3] & (1 << (0x61 & 7)), "$61 is not dirty");

   // FDIVT by zero: the port declines, and the patched instruction (an RTS here) runs instead
   static const uint8_t Divide[] = { 0x20, 0x12, 0xbb };  // JSR FDIVT, with FAC = 0
   Setup(1u << BasicEmuHook_FDivT);
   Cpu = Call(Divide, sizeof Divide);
   Check("a declined hook runs the instruction it replaced",
         Cpu.PC == 0x1003 && BasicEmu.HookCalls[BasicEmuHook_FDivT] == 1 && BasicEmu.HookNative[BasicEmuHook_FDivT] == 1,
         "the ROM's instruction did not run");

   // BASIC banked out: $BA28 is RAM, and no hook runs
   Setup(1u << BasicEmuHook_FMult);
   BasicEmu.Ram[0xba28] = 0x60;
   Cpu6502Write(1, 0x36);
   Cpu = Call(Code, sizeof Code);
   Check("no hook while BASIC is banked out", Cpu.PC == 0x1000 + sizeof Code && BasicEmu.HookCalls[BasicEmuHook_FMult] == 0,
         "the hook ran on RAM");

   // a hook left out of the mask is the ROM's own byte
   Setup(0);
   Cpu = Call(Code, sizeof Code);
   Check("an unhooked entry runs the ROM", BasicEmu.HookCalls[BasicEmuHook_FMult] == 0 && Cpu.PC == 0x1000 + sizeof Code,
         "a hook ran");
}

static void Flush(void)
{
   static const uint16_t Marked[] = { 0x0400, 0x07e7, 0x0800, 0x1800, 0x2000, 0x3f3f, 0x4000, 0x5000 };
   Setup(0);
   for (size_t i = 0; i < sizeof Marked / sizeof Marked[0]; i++) Cpu6502Write(Marked[i], 0xaa);
   memset(&Bus, 0, sizeof Bus);
   BasicEmu.IoLast[0xd00] = 3;     // bank 0
   BasicEmu.IoLast[0x018] = 0x18;  // screen $0400, bitmap $2000
   BasicEmu.IoLast[0x011] = 0x3b;  // bitmap mode
   unsigned N = BasicEmuFlushVideo();
   Check("a bitmap screen flushes its screen and bitmap only",
         N == 4 && Bus.Writes == 4 && Bus.Addr[0] == 0x0400 && Bus.Addr[1] == 0x07e7 && Bus.Addr[2] == 0x2000 &&
            Bus.Addr[3] == 0x3f3f,
         "wrong bytes flushed");
   Check("a flushed byte is clean, and a second flush writes nothing", BasicEmuFlushVideo() == 0, "flushed twice");

   memset(&Bus, 0, sizeof Bus);
   BasicEmu.IoLast[0x018] = 0x10;  // bitmap at $0000: the VIC sees the character ROM at $1000
   N = BasicEmuFlushVideo();
   Check("bank 0's character ROM window is not flushed", N == 1 && Bus.Addr[0] == 0x0800, "flushed $1000-$1FFF");

   memset(&Bus, 0, sizeof Bus);
   BasicEmu.IoLast[0xd00] = 2;     // bank 1, $4000
   BasicEmu.IoLast[0x018] = 0x04;  // screen $4000, characters $4800
   BasicEmu.IoLast[0x011] = 0x1b;
   BasicEmu.IoLast[0x015] = 0x01;
   BasicEmu.Ram[0x43f8] = 0x40;    // sprite 0 at $5000
   N = BasicEmuFlushVideo();
   Check("bank 1 flushes its screen and sprite 0", N == 2 && Bus.Addr[0] == 0x4000 && Bus.Addr[1] == 0x5000,
         "wrong bytes flushed");
   Check("everything else waits for a full flush", BasicEmuFlushAll() == 1, "dirty bytes lost");

   // RAM under I/O: DMA would reach the chip, not the RAM, so the byte stays dirty
   Setup(0);
   Cpu6502Write(1, 0x34);
   Cpu6502Write(0xd020, 0x00);
   BasicEmu.IoLast[0xd00] = 0;     // bank 3, $C000
   BasicEmu.IoLast[0x018] = 0x04;  // characters at $D000
   BasicEmu.IoLast[0x011] = 0x1b;
   Cpu6502Write(0xd000, 0xaa);
   memset(&Bus, 0, sizeof Bus);
   N = BasicEmuFlushVideo() + BasicEmuFlushAll();
   Check("RAM under I/O is never flushed to the bus, and stays dirty",
         N == 0 && Bus.Writes == 0 && (BasicEmu.Dirty[0xd020 >> 3] & (1 << (0xd020 & 7))) &&
            (BasicEmu.Dirty[0xd000 >> 3] & 1),
         "a RAM byte under I/O went to the bus");
}

int main(void)
{
   Banking();
   ModifyOnIo();
   Hooks();
   Flush();
   return Failed;
}
