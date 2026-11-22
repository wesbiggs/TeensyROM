// SPDX-License-Identifier: MIT
// usb_kbd_harness: the USB Keyboard Special IO (IOH_UsbKbd.c), driven from stdin one command
// a line, as the C64, the main loop and the USB core would drive it:
//   w REG VAL      an IO1 write of VAL (hex) at $DE00+REG (hex)
//   r REG          an IO1 read, printing "read XX" or "open"
//   poll           the polling handler
//   init           the handler starts
//   end            UsbKbd_End: the TR menu or a remote launch takes over
//   ms N           N milliseconds pass
//   host 0|1       a USB host has configured the device, or not
//   leds N         the host sets its LEDs to N
//   onsend N R V   while the Nth report from now is being sent, an IO1 write of V at R lands
//                  (the C64 writing while the polling handler is mid-queue)
// Output: "report MM K1 K2 K3 K4 K5 K6" (hex) for each report the core is given, with
// " (no host)" when it has no host to send it to.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define FLASHMEM

struct stcIOHandlers
{
  char Name[20];
  void (*InitHndlr)();
  void (*IO1Hndlr)(uint8_t Address, bool R_Wn);
  void (*IO2Hndlr)(uint8_t Address, bool R_Wn);
  void (*ROMLHndlr)(uint32_t Address, bool R_Wn);
  void (*ROMHHndlr)(uint32_t Address, bool R_Wn);
  void (*PollingHndlr)();
  void (*CycleHndlr)(bool R_Wn);
};

static uint32_t Now = 1000;
static uint32_t millis() { return Now; }

static uint8_t BusData;
static int Driven;
static uint8_t DataPortWaitRead() { return BusData; }
static void DataPortWriteWaitLog(uint8_t Data) { Driven = Data; }
static void TraceLogAddValidData(uint8_t) {}

uint8_t keyboard_modifier_keys;
uint8_t keyboard_keys[6];
volatile uint8_t keyboard_leds;
volatile uint8_t usb_configuration = 1;

struct DeferredWrite { int Countdown; uint8_t Reg, Val; };
static std::vector<DeferredWrite> Deferred;
static void IOWrite(uint8_t Reg, uint8_t Val);

static int usb_keyboard_send()
{
   std::vector<DeferredWrite> Due;
   for (auto It = Deferred.begin(); It != Deferred.end();)
   {
      if (--It->Countdown == 0) { Due.push_back(*It); It = Deferred.erase(It); }
      else ++It;
   }
   for (const DeferredWrite &W : Due) IOWrite(W.Reg, W.Val);
   std::printf("report %02x", keyboard_modifier_keys);
   for (uint8_t Key : keyboard_keys) std::printf(" %02x", Key);
   std::printf(usb_configuration ? "\n" : " (no host)\n");
   return usb_configuration ? 0 : -1;
}

#include "../MinimalBoot/Common/IO_Handlers/IOH_UsbKbd.c"

static void IOWrite(uint8_t Reg, uint8_t Val)
{
   BusData = Val;
   IOHndlr_UsbKbd.IO1Hndlr(Reg, false);
}

int main()
{
   std::string Line;
   while (std::getline(std::cin, Line))
   {
      std::istringstream In(Line);
      std::string Cmd, A, B, C;
      In >> Cmd >> A >> B >> C;
      if (Cmd == "w") IOWrite((uint8_t)std::strtoul(A.c_str(), NULL, 16), (uint8_t)std::strtoul(B.c_str(), NULL, 16));
      else if (Cmd == "onsend")
         Deferred.push_back({(int)std::strtoul(A.c_str(), NULL, 10), (uint8_t)std::strtoul(B.c_str(), NULL, 16),
                             (uint8_t)std::strtoul(C.c_str(), NULL, 16)});
      else if (Cmd == "r")
      {
         Driven = -1;
         IOHndlr_UsbKbd.IO1Hndlr((uint8_t)std::strtoul(A.c_str(), NULL, 16), true);
         if (Driven < 0) std::printf("open\n");
         else std::printf("read %02x\n", Driven);
      }
      else if (Cmd == "poll") IOHndlr_UsbKbd.PollingHndlr();
      else if (Cmd == "init") IOHndlr_UsbKbd.InitHndlr();
      else if (Cmd == "end") UsbKbd_End();
      else if (Cmd == "ms") Now += std::strtoul(A.c_str(), NULL, 10);
      else if (Cmd == "host") usb_configuration = A == "1";
      else if (Cmd == "leds") keyboard_leds = (uint8_t)std::strtoul(A.c_str(), NULL, 10);
      else { std::fprintf(stderr, "unknown command: %s\n", Line.c_str()); return 1; }
      std::fflush(stdout);
   }
   return 0;
}
