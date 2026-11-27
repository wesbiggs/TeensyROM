// SPDX-License-Identifier: MIT
// tdrive_reu_harness: the REU registers under "TDrive+REU" (IO2Hndlr_TDriveREU), where the bus snoop is shared with the
// replacement KERNAL's check, driven from stdin one command a line:
//   kernal 0       the KERNAL check holds the bus snoop, as a PRG launch under TDrive+REU leaves it
//   d VALUE        the byte the C64 drives on the next IO2 write
//   w OFFSET       an IO2 write of that byte
//   r OFFSET       an IO2 read
//   ff00 0         the CPU stores to $FF00: prints "transfer" if the REU started one, else "none"
//   snoop 0        prints who holds the bus snoop: "kernal", "armed" (the $FF00 trigger, KERNAL still served) or "none"
#include <cstdint>
#include <cstdio>
#include <cstring>

#define FLASHMEM
#define FASTRUN
#define RAM_Bank_Size 0x2000
#define MaxPathLength 256
#define SetIRQAssert
#define SetIRQDeassert
#define SetDMAAssert
#define SetDMADeassert
#define ReadGPIO6 0
#define GP6_Phi2(Port) ((void)(Port), Phi2Edge())
#define ARM_DWT_CYCCNT 0u
#define WaitUntil_nS(nS)
#define GPIO6_ISR Discard
#define CORE_PIN1_BITMASK 0
#define NVIC_CLEAR_PENDING(Irq)
#define IRQ_GPIO6789 0

struct StructMenuItem;
enum DMA_Trans_RnW { DMA_READ, DMA_WRITE };
enum DMA_Addr_Mode { DMA_ADDR_INCREMENT, DMA_ADDR_FIXED };
struct stcIOHandlers
{
   char Name[20];
   void (*InitHndlr)();
   void (*IO1Hndlr)(uint8_t Address, bool R_Wn);
   void (*IO2Hndlr)(uint8_t Address, bool R_Wn);
   void (*ROMLHndlr)(uint32_t Address);
   void (*ROMHHndlr)(uint32_t Address);
   void (*PollingHndlr)();
   void (*CycleHndlr)();
};
static struct { template <typename... Args> void printf(const char *, Args...) {} } Serial;

static uint32_t Discard, StartCycCnt, LastCycCnt;
static bool Phi2;
static bool Phi2Edge() { return Phi2 = !Phi2; }
static uint32_t micros() { return 0; }
static const int eepAdREUFilename = 0;
static void EEPreadStr(int, char *Out) { *Out = 0; }
static bool (*fBusSnoop)(uint16_t Address, bool R_Wn), (*PendingfBusSnoop)(uint16_t Address, bool R_Wn);
void (*fSpecialBtnChange)(bool Up_nDn);
bool DMA_RnW, DMA_FixC64Addr;
uint32_t DMA_Length, DMA_Count, DMA_StartAddr;
StructMenuItem *DriveDirMenu;
bool DMAByte(uint8_t *) { return true; }
static uint8_t AllocRAMBanks(uint8_t **, uint8_t, uint8_t) { return 0; }
static void FreeRAMBanks(uint8_t **, uint8_t *Count) { *Count = 0; }
static void LoadRAMBanks(uint8_t **, uint8_t, const char *) {}
static void SaveRAMBanks(uint8_t **, uint8_t, const char *) {}
static void InitUSBMouse() {}
static void InitTRBlock() {}
static void PollUSBMouse() {}
static void PollTRBlock() {}
static void TRPageIO(uint8_t, bool) {}
#define Fab04_REU
#include "../MinimalBoot/Common/Menu_Regs.h"
uint8_t CurrentIOHandler = IOH_REU;

static void DataPortWriteWaitLog(uint8_t) {}
static uint8_t WriteData;
static uint8_t DataPortWaitRead() { return WriteData; }

#include "../MinimalBoot/Common/IO_Handlers/IOH_REU.c"

#define VIO_WIN_OFS 0x80
static void InitHndlr_TDrive() {}
static void PollingHndlr_TDrive() {}
static void IO1Hndlr_TDrive(uint8_t, bool) {}
static void IO2Hndlr_TDrive(uint8_t, bool) {}
uint32_t RAM2BytesFree() { return 0; }
#include "../MinimalBoot/Common/IO_Handlers/IOH_TDriveREU.c"

static uint8_t BankRAM[MAX_REU_BANKS][RAM_Bank_Size];
static bool KernalCheck(uint16_t, bool) { return false; }

int main()
{
   char Command[16];
   unsigned Value;

   NumREU_Banks = 1;
   REU_Bank[0] = BankRAM[0];
   while (scanf("%15s %x", Command, &Value) == 2)
   {
      if (!strcmp(Command, "kernal")) fBusSnoop = &KernalCheck;
      else if (!strcmp(Command, "d")) WriteData = Value;
      else if (!strcmp(Command, "w")) IOHndlr_TDriveREU.IO2Hndlr(Value, false);
      else if (!strcmp(Command, "r")) IOHndlr_TDriveREU.IO2Hndlr(Value, true);
      else if (!strcmp(Command, "ff00")) printf("%s\n", fBusSnoop && fBusSnoop(0xff00, false) ? "transfer" : "none");
      else if (!strcmp(Command, "snoop"))
         printf("%s\n", fBusSnoop == &KernalCheck ? "kernal" : fBusSnoop == &TDREU_Snoop ? "armed" : fBusSnoop ? "other" : "none");
      else return 1;
   }
   return 0;
}
