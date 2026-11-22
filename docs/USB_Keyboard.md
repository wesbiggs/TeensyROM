# The C64 keyboard as a USB keyboard

A special firmware build, "TeensyROM + USB keyboard", adds a Special IO, **USB Keyboard**,
that lets a C64 or C128 program type into whatever the TeensyROM's micro-USB port is
plugged into: a PC, a Mac, a MiSTer. The computer sees a standard USB keyboard and needs no
driver or host software.

The Special IO is a few registers in IO1. A program writes HID key presses and releases to
them; it doesn't need to know anything about USB.
[PETSKEY Remote](https://github.com/wesbiggs/petskey-remote) uses it to turn the whole C64
keyboard into a USB keyboard.

This is not in the release images. Build it with:

```
node tools/build-firmware.mjs --target tr-plus --usb-keyboard      # or --target tr
```

It writes `TeensyROM+_<ver>_usbkbd_full.hex`.

## Using it with PETSKEY Remote

1. Flash the `_usbkbd` image and plug the TeensyROM's micro-USB port into the computer.
2. In the TR menu, under Settings, set **Special IO** to **USB Keyboard**. It comes after
   GeoRAM in the list.
3. Start PETSKEY REMOTE. It finds the registers on its own and types over USB; no Ethernet,
   host or settings needed. Its status line shows whether a computer has set up the
   keyboard.

**RUN/STOP + RESTORE** quits and releases every key on the USB side. If the C64 is reset
with a key down, the key is released within 3 seconds.

## The registers

| Address | Write | Read |
|---|---|---|
| `$DE00` | press HID usage *n* | status: bit 0 = a USB host has set up the keyboard, bit 1 = event queue full |
| `$DE01` | release usage *n* | the host's LEDs: bit 0 Num Lock, 1 Caps Lock, 2 Scroll Lock, 3 Compose, 4 Kana |
| `$DE02` | any value: release every key | interface version, 1 |
| `$DE03` | any value: heartbeat | 0 |
| `$DE04`–`$DE06` | — | `"TRK"`, so a program can tell the registers are there |

The rest of IO1, and all of IO2, is left open. Reads have no side effects, so an indexed
store (`STA $DE00,X`, which reads before it writes) is safe.

- **Usages** come from HID's keyboard page: `$04` is A, `$28` Return, `$E0`–`$E7` the
  modifiers (left Ctrl, Shift, Alt, GUI, then the right ones). `$04`–`$7F` and
  `$E0`–`$E7` are what the boot keyboard report carries; anything else is ignored.
- **Presses are counted per usage.** A key goes up when its releases match its presses, so
  two keys of a program's own that both mean Shift need no counting on the C64. Writing
  `$DE02` clears every count.
- **Six keys plus modifiers.** A seventh held key waits for a free slot, then appears.
- **Heartbeat:** with keys down and no register written for 3 seconds, every key is
  released. A program that holds keys writes something about every second: `$DE03` if
  nothing else.
- **Queue:** writes wait in a 64-entry queue that the main loop empties on every pass. A
  program writing in a tight loop can wait while bit 1 of `$DE00` is set; a write to a full
  queue is dropped, except a release-all, which still takes effect and drops what was
  queued before it.
- **No USB host:** the writes still count, and the report goes out once a host sets up the
  keyboard.

A minimal program that types "A":

```
   lda $de04          ; "TRK"?
   cmp #$54
   bne nokbd
   lda #$04           ; A
   sta $de00          ; press
   sta $de01          ; release
```

## What it is

- **USB device:** the stock Serial + MIDI interfaces plus a boot-protocol keyboard, product
  ID 16C0:048D instead of 16C0:0489. The serial port may get a new COM number on Windows.
  The ID is provisional: PJRC has not allocated it.
- **Firmware:** `IO_Handlers/IOH_UsbKbd.c`. The IO1 handler only stores each write in the
  queue; the polling handler builds the report and sends it. Leaving the handler (the TR
  menu, a remote launch) releases every key. Without `--usb-keyboard` none of this is
  compiled.
- **Its Special IO number** is the last of all the handlers, not the one after GeoRAM, so
  that a stock image flashed over this one, with USB Keyboard still selected, finds the
  saved number out of range and loads no handler instead of the wrong one.

## Differences from a real keyboard

- HID sends key positions, so the computer's layout setting decides the characters. A
  program that wants `"` on a computer set to US sends Shift + `'` (`$34`); set to UK, the
  same keys give `@`.
- Keys with no usage up to `$7F` can't be sent: the media keys, and the keys that only have
  a usage above it (international RO, keypad comma, volume).

## Not yet tested on hardware

Host tests only (`Source/Teensy/tests/usb-kbd.test.js`). Still to check on a board:
enumeration on Windows, macOS and Linux, with `trserial`, the TeensyROM UI and
`npm run flash` finding the new ID; bus timing (cartridge games with the device port
plugged in, then fast typing in PETSKEY); every key against petskey-remote's
`netkbd_host.py --dry-run`; the Special IO list stepping into and out of USB Keyboard both
ways; and a MiSTer.
