// SPDX-License-Identifier: MIT

// USB Keyboard: the C64 types on whatever the Teensy's micro-USB port is plugged into, a
//  PC, a Mac or a MiSTer, as a boot-protocol keyboard. Only in the USB_MIDI_SERIAL_KEYBOARD
//  build (build-firmware.mjs --usb-keyboard), which adds the keyboard to the device port.
//  A C64 program writes HID keyboard-page usages; it knows nothing of the USB side
//  (docs/USB_Keyboard.md). PETSKEY Remote (github.com/wesbiggs/petskey-remote) is one.
//
//    $DE00  W: press usage n      R: status, bit 0 a USB host has set up the keyboard,
//                                    bit 1 the event queue is full
//    $DE01  W: release usage n    R: the host's LEDs (bit 0 Num, 1 Caps, 2 Scroll Lock)
//    $DE02  W: release every key  R: interface version
//    $DE03  W: heartbeat          R: 0
//    $DE04-$DE06                  R: "TRK"
//
//  Presses are counted per usage: a key goes up when its releases match its presses.
//  Usages are $04-$7F and the modifiers $E0-$E7, the range of Teensyduino's boot report.
//  Keys held with no register written for UsbKbd_SilencemS are released, so a C64 that
//  is reset or crashes with a key down cannot leave it repeating on the computer.

void InitHndlr_UsbKbd();
void IO1Hndlr_UsbKbd(uint8_t Address, bool R_Wn);
void PollingHndlr_UsbKbd();

stcIOHandlers IOHndlr_UsbKbd =
{
  "USB Keyboard",        //Name of handler
  &InitHndlr_UsbKbd,     //Called once at handler startup
  &IO1Hndlr_UsbKbd,      //IO1 R/W handler
  NULL,                  //IO2 R/W handler
  NULL,                  //ROML Read handler, in addition to any ROM data sent
  NULL,                  //ROMH Read handler, in addition to any ROM data sent
  &PollingHndlr_UsbKbd,  //Polled in main routine
  NULL,                  //called at the end of EVERY c64 cycle
};

#define UsbKbd_RegPress     0
#define UsbKbd_RegRelease   1
#define UsbKbd_RegAllUp     2
#define UsbKbd_RegHeart     3
#define UsbKbd_NumRegs      7
#define UsbKbd_Version      1
#define UsbKbd_StatHost     0x01
#define UsbKbd_StatFull     0x02
#define UsbKbd_QueueSize    64     // a power of 2; the main loop empties it every pass, and
                                   //  PETSKEY writes at most a few events per keyboard scan
#define UsbKbd_SilencemS    3000   // PETSKEY writes a heartbeat twice a second
#define UsbKbd_FirstUsage   0x04   // 1-3 are the report's error codes
#define UsbKbd_LastKey      0x7f   // Usage Maximum of the keys array in Teensyduino's descriptor
#define UsbKbd_FirstMod     0xe0
#define UsbKbd_NumMods      8
#define UsbKbd_Slots        6

// Read by the IO ISR as they are: the main loop keeps them up to date
static volatile uint8_t UsbKbd_Regs[UsbKbd_NumRegs] = {0, 0, UsbKbd_Version, 0, 'T', 'R', 'K'};

// (register << 8) | data, written by the IO ISR at Head, read by the main loop at Tail
static volatile uint16_t UsbKbd_Queue[UsbKbd_QueueSize];
static volatile uint8_t UsbKbd_Head, UsbKbd_Tail;
static volatile bool UsbKbd_AllUpPending;        // a release-all that found the queue full
static volatile uint32_t UsbKbd_LastWritemS;

// The main loop's
static uint8_t UsbKbd_KeyCount[UsbKbd_LastKey + 1];
static uint8_t UsbKbd_ModCount[UsbKbd_NumMods];
static uint8_t UsbKbd_Order[UsbKbd_LastKey + 1];  // held keys, in press order
static uint8_t UsbKbd_NumHeld;
static bool UsbKbd_Unsent;                        // the report changed and no host has it yet

void IO1Hndlr_UsbKbd(uint8_t Address, bool R_Wn)
{
   if (R_Wn)
   {
      if (Address < UsbKbd_NumRegs) DataPortWriteWaitLog(UsbKbd_Regs[Address]);
      return;
   }
   if (Address > UsbKbd_RegHeart) return;
   uint8_t Data = DataPortWaitRead();
   TraceLogAddValidData(Data);
   UsbKbd_LastWritemS = millis();
   uint8_t Head = UsbKbd_Head;
   uint8_t Next = (Head + 1) & (UsbKbd_QueueSize - 1);
   if (Next == UsbKbd_Tail)
   {
      if (Address == UsbKbd_RegAllUp) UsbKbd_AllUpPending = true;
      return;
   }
   UsbKbd_Queue[Head] = (Address << 8) | Data;
   UsbKbd_Head = Next;
   if (((Next + 1) & (UsbKbd_QueueSize - 1)) == UsbKbd_Tail) UsbKbd_Regs[0] |= UsbKbd_StatFull;
}

FLASHMEM static void UsbKbd_Send()
{
   keyboard_modifier_keys = 0;
   for (uint8_t Mod = 0; Mod < UsbKbd_NumMods; Mod++)
      if (UsbKbd_ModCount[Mod]) keyboard_modifier_keys |= 1 << Mod;
   for (uint8_t Slot = 0; Slot < UsbKbd_Slots; Slot++)
      keyboard_keys[Slot] = Slot < UsbKbd_NumHeld ? UsbKbd_Order[Slot] : 0;
   // with no host, usb_keyboard_send() returns at once; the report goes out when one comes
   UsbKbd_Unsent = (usb_keyboard_send() != 0);
}

FLASHMEM static void UsbKbd_ReleaseAll()
{
   memset(UsbKbd_KeyCount, 0, sizeof(UsbKbd_KeyCount));
   memset(UsbKbd_ModCount, 0, sizeof(UsbKbd_ModCount));
   UsbKbd_NumHeld = 0;
   UsbKbd_Send();
}

// True when the report changed
FLASHMEM static bool UsbKbd_Press(uint8_t Usage)
{
   if (Usage >= UsbKbd_FirstMod && Usage < UsbKbd_FirstMod + UsbKbd_NumMods)
   {
      uint8_t *Count = &UsbKbd_ModCount[Usage - UsbKbd_FirstMod];
      if (*Count == 0xff) return false;
      return (*Count)++ == 0;
   }
   if (Usage < UsbKbd_FirstUsage || Usage > UsbKbd_LastKey) return false;
   if (UsbKbd_KeyCount[Usage] == 0xff) return false;
   if (UsbKbd_KeyCount[Usage]++) return false;
   UsbKbd_Order[UsbKbd_NumHeld++] = Usage;
   return UsbKbd_NumHeld <= UsbKbd_Slots;   // a seventh key waits for a slot
}

FLASHMEM static bool UsbKbd_Release(uint8_t Usage)
{
   if (Usage >= UsbKbd_FirstMod && Usage < UsbKbd_FirstMod + UsbKbd_NumMods)
   {
      uint8_t *Count = &UsbKbd_ModCount[Usage - UsbKbd_FirstMod];
      if (*Count == 0) return false;
      return --(*Count) == 0;
   }
   if (Usage < UsbKbd_FirstUsage || Usage > UsbKbd_LastKey) return false;
   if (UsbKbd_KeyCount[Usage] == 0 || --UsbKbd_KeyCount[Usage]) return false;
   uint8_t Pos;
   for (Pos = 0; UsbKbd_Order[Pos] != Usage; Pos++);
   memmove(&UsbKbd_Order[Pos], &UsbKbd_Order[Pos + 1], UsbKbd_NumHeld - Pos - 1);
   UsbKbd_NumHeld--;
   return Pos < UsbKbd_Slots;
}

FLASHMEM void UsbKbd_End()
{  // the handler going away (the TR menu, a remote launch): nothing stays held on the computer
   if (UsbKbd_NumHeld || keyboard_modifier_keys || UsbKbd_Unsent) UsbKbd_ReleaseAll();
}

FLASHMEM void InitHndlr_UsbKbd()
{
   UsbKbd_Head = UsbKbd_Tail = 0;
   UsbKbd_AllUpPending = false;
   UsbKbd_LastWritemS = millis();
   UsbKbd_Regs[0] = usb_configuration ? UsbKbd_StatHost : 0;
   UsbKbd_Regs[1] = keyboard_leds;
   UsbKbd_ReleaseAll();
}

FLASHMEM void PollingHndlr_UsbKbd()
{
   if (UsbKbd_AllUpPending)
   {  // the queue is full, so the ISR adds nothing to it until Tail moves
      UsbKbd_Tail = UsbKbd_Head;
      UsbKbd_AllUpPending = false;
      UsbKbd_ReleaseAll();
   }
   uint8_t Tail = UsbKbd_Tail;
   while (Tail != UsbKbd_Head)
   {
      uint16_t Entry = UsbKbd_Queue[Tail];
      Tail = (Tail + 1) & (UsbKbd_QueueSize - 1);
      UsbKbd_Tail = Tail;
      uint8_t Data = Entry & 0xff;
      switch (Entry >> 8)
      {
         case UsbKbd_RegPress:   if (UsbKbd_Press(Data)) UsbKbd_Send(); break;
         case UsbKbd_RegRelease: if (UsbKbd_Release(Data)) UsbKbd_Send(); break;
         case UsbKbd_RegAllUp:   UsbKbd_ReleaseAll(); break;
      }
   }
   if ((UsbKbd_NumHeld || keyboard_modifier_keys) && millis() - UsbKbd_LastWritemS >= UsbKbd_SilencemS)
      UsbKbd_ReleaseAll();
   if (UsbKbd_Unsent && usb_configuration) UsbKbd_Send();
   UsbKbd_Regs[0] = usb_configuration ? UsbKbd_StatHost : 0;
   UsbKbd_Regs[1] = keyboard_leds;
}
