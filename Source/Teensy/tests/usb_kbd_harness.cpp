// SPDX-License-Identifier: MIT
// usb_kbd_harness: UsbKbd.c, PETSKEY Remote's USB keyboard, driven from stdin one command
// a line, as the SwiftLink handler and the USB core would drive it:
//   start            AT_DT's "ATDT USB": UsbKbd_Start, prints "connected" or "failed"
//   feed HH HH ...   bytes from the C64, hex
//   ms N             N milliseconds pass
//   poll             the polling handler's UsbKbd_Poll
//   end              UsbKbd_End (+++, reset, handler teardown)
//   host 0|1         a USB host has configured the device, or not
//   isusb TEXT       UsbKbd_IsUsbHost(TEXT), prints yes or no
// Output: "report MM K1 K2 K3 K4 K5 K6" for each HID report sent (hex), "rx TEXT" for each
// line queued for the C64.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#define FLASHMEM
#define PROGMEM
#define Printf_dbg_sw(...)

static uint32_t Now = 1000;
static uint32_t millis() { return Now; }

uint8_t keyboard_modifier_keys;
uint8_t keyboard_keys[6];
volatile uint8_t usb_configuration = 1;

static int usb_keyboard_send()
{
   std::printf("report %02x", keyboard_modifier_keys);
   for (uint8_t Key : keyboard_keys) std::printf(" %02x", Key);
   std::printf("\n");
   return usb_configuration ? 0 : -1;
}

static void AddToPETSCIIStrToRxQueueLN(const char *s) { std::printf("rx %s\n", s); }

#include "../MinimalBoot/Common/IO_Handlers/UsbKbd.c"

int main()
{
   std::string Line;
   while (std::getline(std::cin, Line))
   {
      std::istringstream In(Line);
      std::string Cmd;
      In >> Cmd;
      if (Cmd == "start") std::printf(UsbKbd_Start() ? "connected\n" : "failed\n");
      else if (Cmd == "feed")
      {
         std::string Hex;
         while (In >> Hex) UsbKbd_Feed((uint8_t)std::strtoul(Hex.c_str(), NULL, 16));
      }
      else if (Cmd == "ms") { uint32_t N; In >> N; Now += N; }
      else if (Cmd == "poll") UsbKbd_Poll();
      else if (Cmd == "end") UsbKbd_End();
      else if (Cmd == "host") { int On; In >> On; usb_configuration = On; }
      else if (Cmd == "isusb")
      {
         std::string Text;
         In >> Text;
         std::printf(UsbKbd_IsUsbHost(Text.c_str()) ? "yes\n" : "no\n");
      }
      else { std::fprintf(stderr, "unknown command: %s\n", Line.c_str()); return 1; }
      std::fflush(stdout);
   }
   return 0;   //no UsbKbd_End: its release report would land in every test's output
}
