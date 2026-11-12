// SPDX-License-Identifier: MIT

// PETSKEY Remote over USB: the C64 keyboard as a USB keyboard on the Teensy's device port.
//  PETSKEY REMOTE (github.com/wesbiggs/petskey-remote) dials "ATDT USB" and sends one byte
//  per key press or release through the SwiftLink. This is its host program, netkbd_host.py's
//  Session and Translator, sending boot-protocol HID reports instead of uinput events, so a
//  key does here what it does on the Pi. Built only with USB_MIDI_SERIAL_KEYBOARD
//  (build-firmware.mjs --usb-keyboard). Everything runs from the SwiftLink polling handler.
//
//  C64 -> TR, PETSKEY protocol 2:
//    $F0 v f      hello: version, flags. Starts a session; anything before it is ignored
//    $F1 lo hi .. the keymap: for each of the 90 key codes, plain then SHIFT binding,
//                 2 bytes each: flags (bit 7 defined, bit 0 sets the shift state, bit 1 that
//                 state) and a Linux key code, 0 for none
//    $00-$5F      key down        $80-$DF  key up (code + $80)
//    $FE          heartbeat       $FC      bye: release everything, wait for the next hello

#include "UsbKbdKeys.h"

#define UsbKbd_NumCodes      90
#define UsbKbd_MapBytes      (UsbKbd_NumCodes*4)
#define UsbKbd_CodeCaps      89     // C128 CAPS LOCK, a locking key: each change is one tap
#define UsbKbd_LinuxLShift   42
#define UsbKbd_LinuxRShift   54
#define UsbKbd_SilenceMillis 3000   // the C64 sends a heartbeat twice a second; a reset C64 sends none
#define UsbKbd_ProtoKeymap   2      // first protocol whose C64 sends its keymap

#define UsbKbd_BindDefined   0x80
#define UsbKbd_BindSetsShift 0x01
#define UsbKbd_BindShiftOn   0x02
#define UsbKbd_Down          0x80   // DownFlags: the key is down; bits 0-1 are its binding's

enum enUsbKbdState
{
   UsbKbd_WaitHello,
   UsbKbd_Hello,
   UsbKbd_MapLen,
   UsbKbd_MapData,
   UsbKbd_Live,
};

struct stcUsbKbd
{
   uint8_t  Map[UsbKbd_MapBytes];
   uint8_t  DownFlags[UsbKbd_NumCodes];
   uint8_t  DownKey[UsbKbd_NumCodes];       // the Linux key it pressed, 0 for none
   uint8_t  Counts[256];                    // per Linux key: how many C64 keys hold it down
   uint8_t  Overrides[UsbKbd_NumCodes];     // codes down whose binding sets the shift state, in press order
   uint8_t  NumOverrides;
   bool     SentShift;
   bool     HaveMap;
   bool     SilenceReleased;
   uint8_t  State;
   uint8_t  Hello[2];
   uint8_t  NumHello;
   uint16_t MapLen, MapPos;
   uint32_t LastRxMillis;
};

bool UsbKbdConnected = false;
static stcUsbKbd *UsbKbd = NULL;   // RAM2, only while connected

//_____________________________________HID report_____________________________________________

FLASHMEM static void UsbKbd_Emit(uint8_t LinuxKey, bool Pressed)
{
   uint8_t Usage = UsbKbd_LinuxToHID[LinuxKey];
   if (Usage == 0) return;
   if (Usage >= 0xe0)
   {
      uint8_t Bit = 1 << (Usage - 0xe0);
      uint8_t Mods = Pressed ? (keyboard_modifier_keys | Bit) : (keyboard_modifier_keys & ~Bit);
      if (Mods == keyboard_modifier_keys) return;
      keyboard_modifier_keys = Mods;
   }
   else
   {
      uint8_t Slot;
      for (Slot = 0; Slot < 6 && keyboard_keys[Slot] != Usage; Slot++);
      if (Pressed)
      {
         if (Slot < 6) return;
         for (Slot = 0; Slot < 6 && keyboard_keys[Slot] != 0; Slot++);
         if (Slot == 6) return;   // a seventh key: boot protocol has no room, like any 6KRO keyboard
         keyboard_keys[Slot] = Usage;
      }
      else
      {
         if (Slot == 6) return;
         for (; Slot < 5; Slot++) keyboard_keys[Slot] = keyboard_keys[Slot + 1];
         keyboard_keys[5] = 0;
      }
   }
   usb_keyboard_send();
}

FLASHMEM static void UsbKbd_ClearReport()
{
   bool Any = keyboard_modifier_keys != 0;
   for (uint8_t Slot = 0; Slot < 6; Slot++) if (keyboard_keys[Slot]) Any = true;
   if (!Any) return;
   keyboard_modifier_keys = 0;
   memset(keyboard_keys, 0, 6);
   usb_keyboard_send();
}

//_____________________________________Translator (netkbd_host.py Translator)_________________

FLASHMEM static uint8_t UsbKbd_Binding(uint8_t Code, bool Shifted, uint8_t *Key)
{  //flags, or 0 if the keymap does not define it
   const uint8_t *Entry = &UsbKbd->Map[Code*4 + (Shifted ? 2 : 0)];
   *Key = Entry[1];
   return (Entry[0] & UsbKbd_BindDefined) ? Entry[0] : 0;
}

// Keys that act as SHIFT: they set the state the SHIFT bindings look at, and are sent as
//  the one virtual left shift
FLASHMEM static bool UsbKbd_IsShiftCode(uint8_t Code)
{
   uint8_t Key;
   uint8_t Flags = UsbKbd_Binding(Code, false, &Key);
   if (!Flags || (Key != UsbKbd_LinuxLShift && Key != UsbKbd_LinuxRShift)) return false;
   return (Flags & (UsbKbd_BindSetsShift | UsbKbd_BindShiftOn)) != (UsbKbd_BindSetsShift | UsbKbd_BindShiftOn);
}

FLASHMEM static bool UsbKbd_PhysicalShift()
{
   for (uint8_t Code = 0; Code < UsbKbd_NumCodes; Code++)
      if (UsbKbd->DownFlags[Code] && UsbKbd_IsShiftCode(Code)) return true;
   return false;
}

FLASHMEM static void UsbKbd_SyncShift()
{
   bool Want = UsbKbd_PhysicalShift();
   if (UsbKbd->NumOverrides)
      Want = UsbKbd->DownFlags[UsbKbd->Overrides[UsbKbd->NumOverrides - 1]] & UsbKbd_BindShiftOn;
   if (Want == UsbKbd->SentShift) return;
   UsbKbd->SentShift = Want;
   UsbKbd_Emit(UsbKbd_LinuxLShift, Want);
}

FLASHMEM static void UsbKbd_Press(uint8_t Key)
{
   if (UsbKbd->Counts[Key]++ == 0) UsbKbd_Emit(Key, true);
}

FLASHMEM static void UsbKbd_Release(uint8_t Key)
{
   if (UsbKbd->Counts[Key] == 0) return;
   if (--UsbKbd->Counts[Key] == 0) UsbKbd_Emit(Key, false);
}

FLASHMEM static void UsbKbd_Tap(uint8_t Code)
{
   uint8_t Key;
   if (!UsbKbd_Binding(Code, false, &Key) || Key == 0) return;
   UsbKbd_Press(Key);
   UsbKbd_Release(Key);
}

FLASHMEM static void UsbKbd_KeyDown(uint8_t Code)
{
   if (Code == UsbKbd_CodeCaps) return UsbKbd_Tap(Code);
   if (UsbKbd->DownFlags[Code]) return;
   if (UsbKbd_IsShiftCode(Code))
   {
      UsbKbd->DownFlags[Code] = UsbKbd_Down;
      UsbKbd->DownKey[Code] = 0;
      UsbKbd_SyncShift();
      return;
   }
   uint8_t Key = 0, Flags = 0;
   if (UsbKbd_PhysicalShift()) Flags = UsbKbd_Binding(Code, true, &Key);
   if (!Flags) Flags = UsbKbd_Binding(Code, false, &Key);
   if (!Flags) Key = 0;
   UsbKbd->DownFlags[Code] = UsbKbd_Down | (Flags & (UsbKbd_BindSetsShift | UsbKbd_BindShiftOn));
   UsbKbd->DownKey[Code] = Key;
   if (Key == 0) return;
   if (Flags & UsbKbd_BindSetsShift) UsbKbd->Overrides[UsbKbd->NumOverrides++] = Code;
   UsbKbd_SyncShift();
   UsbKbd_Press(Key);
}

FLASHMEM static void UsbKbd_KeyUp(uint8_t Code)
{
   if (Code == UsbKbd_CodeCaps) return UsbKbd_Tap(Code);
   if (!UsbKbd->DownFlags[Code]) return;   // held before the C64 started scanning, or after a release-all
   uint8_t Key = UsbKbd->DownKey[Code];
   UsbKbd->DownFlags[Code] = 0;
   if (Key)
   {
      UsbKbd_Release(Key);
      for (uint8_t Num = 0; Num < UsbKbd->NumOverrides; Num++)
      {
         if (UsbKbd->Overrides[Num] != Code) continue;
         memmove(&UsbKbd->Overrides[Num], &UsbKbd->Overrides[Num + 1], UsbKbd->NumOverrides - Num - 1);
         UsbKbd->NumOverrides--;
         break;
      }
   }
   UsbKbd_SyncShift();
}

FLASHMEM void UsbKbd_ReleaseAll()
{
   if (UsbKbd == NULL) return;
   memset(UsbKbd->DownFlags, 0, sizeof(UsbKbd->DownFlags));
   memset(UsbKbd->Counts, 0, sizeof(UsbKbd->Counts));
   UsbKbd->NumOverrides = 0;
   UsbKbd->SentShift = false;
   UsbKbd_ClearReport();
}

//_____________________________________Session (netkbd_host.py Session)________________________

FLASHMEM static void UsbKbd_Greet(uint8_t Version)
{
   if (Version < UsbKbd_ProtoKeymap) AddToPETSCIIStrToRxQueueLN("This PETSKEY sends no keymap: update it.");
   else if (!usb_configuration) AddToPETSCIIStrToRxQueueLN("Linked to USB keyboard (no USB host).");
   else AddToPETSCIIStrToRxQueueLN("Linked to USB keyboard.");
}

FLASHMEM static void UsbKbd_Byte(uint8_t Byte)
{
   switch (UsbKbd->State)
   {
      case UsbKbd_MapLen:
         UsbKbd->Hello[UsbKbd->NumHello++] = Byte;
         if (UsbKbd->NumHello < 2) return;
         UsbKbd->MapLen = UsbKbd->Hello[0] | (UsbKbd->Hello[1] << 8);
         UsbKbd->MapPos = 0;
         memset(UsbKbd->Map, 0, sizeof(UsbKbd->Map));
         UsbKbd->State = UsbKbd->MapLen ? UsbKbd_MapData : UsbKbd_Live;
         UsbKbd->HaveMap = (UsbKbd->MapLen == 0);
         return;

      case UsbKbd_MapData:   // longer than ours: the rest is for codes this firmware does not know
         if (UsbKbd->MapPos < UsbKbd_MapBytes) UsbKbd->Map[UsbKbd->MapPos] = Byte;
         if (++UsbKbd->MapPos < UsbKbd->MapLen) return;
         UsbKbd->State = UsbKbd_Live;
         UsbKbd->HaveMap = true;
         return;

      case UsbKbd_Hello:
         UsbKbd->Hello[UsbKbd->NumHello++] = Byte;
         if (UsbKbd->NumHello < 2) return;
         UsbKbd->State = UsbKbd_Live;
         UsbKbd->HaveMap = false;
         Printf_dbg_sw("PETSKEY hello: protocol %d, flags %02x\n", UsbKbd->Hello[0], UsbKbd->Hello[1]);
         UsbKbd_Greet(UsbKbd->Hello[0]);
         return;
   }

   if (Byte == 0xf0)   //hello, also a restart while live
   {
      UsbKbd_ReleaseAll();
      UsbKbd->State = UsbKbd_Hello;
      UsbKbd->NumHello = 0;
      return;
   }
   if (UsbKbd->State != UsbKbd_Live) return;   //the modem's echo of ATDT, anything before a hello

   if (Byte == 0xf1)
   {
      UsbKbd_ReleaseAll();
      UsbKbd->State = UsbKbd_MapLen;
      UsbKbd->NumHello = 0;
   }
   else if (Byte == 0xfc)
   {
      UsbKbd_ReleaseAll();
      UsbKbd->State = UsbKbd_WaitHello;
   }
   else if (!UsbKbd->HaveMap) return;   //protocol 1 sends no keymap, and this firmware has none of its own
   else if (Byte < UsbKbd_NumCodes) UsbKbd_KeyDown(Byte);
   else if (Byte >= 0x80 && Byte < 0x80 + UsbKbd_NumCodes) UsbKbd_KeyUp(Byte & 0x7f);
}

//_____________________________________Called from the SwiftLink handler______________________

// "usb", or "usb:<anything>", quoted or not: AT_DT's argument, already in lower case
FLASHMEM bool UsbKbd_IsUsbHost(const char *Host)
{
   while (*Host == '\"') Host++;
   if (strncmp(Host, "usb", 3) != 0) return false;
   return Host[3] == 0 || Host[3] == ':' || Host[3] == '\"';
}

FLASHMEM bool UsbKbd_Start()
{
   if (UsbKbd == NULL) UsbKbd = (stcUsbKbd *)malloc(sizeof(stcUsbKbd));
   if (UsbKbd == NULL) return false;
   memset(UsbKbd, 0, sizeof(stcUsbKbd));
   UsbKbd->State = UsbKbd_WaitHello;
   UsbKbd->LastRxMillis = millis();
   UsbKbd_ClearReport();
   UsbKbdConnected = true;
   return true;
}

FLASHMEM void UsbKbd_Feed(uint8_t Byte)
{
   if (UsbKbd == NULL) return;
   UsbKbd->LastRxMillis = millis();
   UsbKbd->SilenceReleased = false;
   UsbKbd_Byte(Byte);
}

// A C64 reset with a key down sends no key up, so nothing is left held once it goes quiet.
//  A new hello restarts the session; the "connection" stays until +++.
FLASHMEM void UsbKbd_Poll()
{
   if (UsbKbd == NULL || UsbKbd->SilenceReleased) return;
   if (millis() - UsbKbd->LastRxMillis < UsbKbd_SilenceMillis) return;
   UsbKbd->SilenceReleased = true;
   UsbKbd_ReleaseAll();
}

// Hang-up, reset, or the handler going away: nothing stays held on the USB host
FLASHMEM void UsbKbd_End()
{
   UsbKbd_ReleaseAll();
   free(UsbKbd);
   UsbKbd = NULL;
   UsbKbdConnected = false;
}
