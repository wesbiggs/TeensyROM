# Proposal: Special IO Declarations for PRGs on SD/USB

Status: **Draft for discussion, revision 2**

Revision 2 changes the model. A PRG no longer names *a handler*. It declares
the **capabilities** it needs, would like to have, or must not have, and the
firmware picks the handler that best fits. Revision 1 assumed each handler did
one thing. That breaks down for combined handlers such as VDrive+Swift+REU,
which provide several capabilities at once, sometimes with different
parameters (512 KB REU on its own, 128 KB REU when combined with SwiftLink).

## Contents

1. [Problem](#problem)
2. [Goals](#goals)
3. [Part 1: Capabilities and handlers](#part-1-capabilities-and-handlers)
4. [Part 2: The declaration language](#part-2-the-declaration-language)
5. [Part 3: How the firmware picks a handler](#part-3-how-the-firmware-picks-a-handler)
6. [Part 4: Examples](#part-4-examples)
7. [Part 5: Where declarations live](#part-5-where-declarations-live)
8. [Part 6: Precedence between files](#part-6-precedence-between-files)
9. [Part 7: Creating and installing declarations](#part-7-creating-and-installing-declarations)
10. [Part 8: Implementation outline](#part-8-implementation-outline)
11. [Questions to decide](#questions-to-decide)

---

## Problem

Some PRGs only work with particular Special IO: terminal programs need
SwiftLink, MIDI sequencers need a particular MIDI interface, and some demos
need an REU of a minimum size. Some programs need a disk drive, and some only
work with a real 1541 because they upload their own drive code.

Today the user picks one handler from **Settings → Special IO**. That choice is
stored in EEPROM (`eepAdNextIOHndlr`) and applies to every PRG launched
afterwards. Running a program with the wrong handler fails without saying why.

The firmware already has a per-item mechanism, but it covers only the built-in
TR menu:

- `StructMenuItem.IOHndlrAssoc` (`Menu_Regs.h`) holds a handler for each menu item.
- `IOHandlerSelectInit()` (`IOHandlers.ino`), the Kernal Replace pre-start
  check (`StatusFunctions.c`) and `UpdateItemTypeReg()` (`IOH_TeensyROM.c`,
  which sets bit 7 to show an assigned handler) all honour it.
- All three check `IO1[rWRegCurrMenuWAIT] == rmtTeensy` first. This is needed:
  `DriveDirMenu` entries are `malloc`ed in `LoadDirectory()` and
  `SetDriveDirMenuNameType()`, and nothing initialises their `IOHndlrAssoc`.

### Why choosing a handler by name isn't enough

Several handlers are combinations. The firmware has them today, and the VDrive
work adds more:

| Handler | What it provides |
|---|---|
| `RetroReplay` / `RetroReplay_REU` | freezer, or freezer + REU |
| `FCIII` / `FCIII_101_REU` | freezer, or freezer + REU |
| `SuperSnapshotV5` / `SuperSnapshotV5_REU` | freezer, or freezer + REU |
| `GeoRAM` | GeoRAM **and** the USB mouse registers **and** the TR block device (`IOH_GeoRAM.c` calls `InitUSBMouse()` and `InitTRBlock()`) |
| VDrive | virtual drive |
| VDrive+REU | virtual drive + 512 KB REU |
| VDrive+Swift | virtual drive + SwiftLink |
| VDrive+Swift+REU | virtual drive + SwiftLink + **128 KB** REU |

There are combinations because the IO1/IO2 address space and Teensy RAM are
limited, so not every set of features can run together, and some only fit at a
smaller size. A program that says "I need handler `vdrive-swift`" would refuse
to run on VDrive+Swift+REU, which also provides everything it needs. A program
that says "I need an REU" can't say how big, so it might be started with a
128 KB REU that it can't use.

A program's real requirements also combine in several ways:

1. **Required versus nice to have.** "I need SwiftLink; I'll use an REU if
   there is one."
2. **Several requirements at once.** "SwiftLink *and* a disk."
3. **Alternatives.** "A disk, which can be a real IEC drive *or* VDrive."
4. **Things that must not be present.** "No VDrive on device 8, because my
   fastloader uploads code to a real 1541."
5. **Parameters.** "REU of *at least* 512 KB."

## Goals

1. **Stable tokens** for capabilities, and for handlers, that don't change
   between builds or releases.
2. A declaration language that can express required, optional and forbidden
   capabilities, several at once, alternatives, and parameters.
3. A **predictable, documented** way of turning a declaration into one handler
   (or a clear refusal), so that a collection author knows what will happen on
   every build.
4. Declarations that are easy to write, can be shared, and travel with the files.
5. The same answer on every launch path: the menu, autolaunch, hot keys, NFC and
   remote launch from host apps.
6. No changes to the PRG files themselves.

Not in scope: CRTs. Their hardware type is in the CRT header.

---

## Part 1: Capabilities and handlers

There are two layers, each with its own stable tokens:

- A **capability** is something a program can use: `swiftlink`, `reu`,
  `vdrive`, `iec`, `midi`. It is what declarations talk about.
- A **handler** is a concrete Special IO personality in `IOHandler[]`, such as
  `vdrive-swift-reu`. Each handler lists the capabilities it **provides**.
  Declarations normally don't name handlers. The firmware chooses one.

### 1.1 Capability vocabulary

Proposed tokens, which are permanent once released. A capability can have
**parameters**, written `name=value`.

| Capability | Parameters | Meaning |
|---|---|---|
| `swiftlink` | `base` (`de00`, `df00`, `d700`), `int` (`nmi`, `irq`) | 6551 ACIA, as SwiftLink or Turbo232. Today TR provides `base=de00 int=nmi`. |
| `midi` | `type` (`datel`, `sequential`, `passport`, `namesoft`) | MIDI cartridge interface. Also written `midi:datel` (see 2.3). |
| `reu` | `size` (bytes, with `k`/`m` suffix) | 17xx REU at `$DF00`. |
| `georam` | `size` | GeoRAM / NeoRAM at `$DE00`/`$DFxx`. |
| `mouse` | `type` (`1351`) | USB mouse presented as a 1351 (currently comes with the GeoRAM handler). |
| `trblock` | | TR block device registers (`docs/TR_Block_Device.md`). |
| `kernal` | `type` (`custom`) | A replacement KERNAL is mapped in. Mostly used with `forbid`. |
| `trbasic` | | TR BASIC extensions. |
| `vdrive` | `device` (8–11) | TeensyROM virtual drive. KERNAL-level LOAD/SAVE/OPEN works, but drive code (`M-W`/`M-E` to a 1541) does not. |
| `iec` | `device`, `type` (`1541`, `1571`, `1581`, `sd2iec`, `pi1541`, …), `drivecode` (`1541`, `1571`, `1581`, `none`) | A device on the physical IEC bus. TR can't sense this (see 1.3). |
| `disk` | `device` | **Abstract**: any device that KERNAL LOAD/SAVE works with. Satisfied by `vdrive` *or* `iec` (see 1.4). |
| `media` | `src` (`sd`, `usb`, `tr`) | Where the PRG is being launched *from*. Provided by the launch, not by a handler. Mostly used with `forbid`. |
| `machine` | `type` (`c64`, `c128`), `video` (`pal`, `ntsc`) | The host machine, as already detected (`MachineDetect.h`). |
| `tr` | `model` (`tr`, `trplus`) | The TeensyROM hardware, for programs that use TR-specific registers. |

**Drive fidelity** is the part most likely to cause confusion. It comes in three
levels:

1. `disk`: "I call KERNAL `LOAD`/`SAVE`". VDrive, SD2IEC, Pi1541 and a real
   drive all qualify.
2. `iec`: "I need something on the actual serial bus", for example programs
   that talk to the bus directly, or need a second device such as a printer.
3. `iec drivecode=1541`: "I upload code into a 1541's RAM and run it there".
   Real 1541s, Pi1541 and the Ultimate's 1541 emulation qualify. SD2IEC and
   VDrive do not.

### 1.2 What each handler provides

Each handler lists what it provides, as a string in the **same syntax as
declarations**, so there is only one grammar to parse and document:

```c
struct stcIOHandlers
{
  char Name[IOHNameLength];   // display name, may change
  const char *Token;          // stable handler ID, never changes once released
  const char *Provides;       // capability list, e.g. "swiftlink base=de00 int=nmi; reu size=128k"
  uint8_t DeclFlags;          // IOHDecl_Allowed: may be chosen by the resolver
  void (*InitHndlr)();
  ...
};
```

Proposed contents:

| Handler token | `Provides` | Resolver may choose it? |
|---|---|---|
| `none` | *(nothing)* | yes |
| `swiftlink` | `swiftlink base=de00 int=nmi` | yes |
| `midi-datel` | `midi type=datel` | yes |
| `midi-seq` | `midi type=sequential` | yes |
| `midi-passport` | `midi type=passport` | yes |
| `midi-namesoft` | `midi type=namesoft` | yes |
| `reu` | `reu size=512k` *(the actual size, see note)* | yes (`Fab04_REU` builds) |
| `kernal` | `kernal type=custom` | only if named with `handler` (see 2.4) |
| `georam` | `georam size=<actual>; mouse type=1351; trblock` | yes |
| `debug` | | no |
| `tr-basic` | `trbasic` | yes |
| `vdrive` | `vdrive device=8` | yes |
| `vdrive-reu` | `vdrive device=8; reu size=512k` | yes |
| `vdrive-swift` | `vdrive device=8; swiftlink base=de00 int=nmi` | yes |
| `vdrive-swift-reu` | `vdrive device=8; swiftlink base=de00 int=nmi; reu size=128k` | yes |
| `retroreplay`, `fcIII`, `ssv5`, `actionreplay`, `*-reu`, `teensyrom` | (freezers / menu) | no |

Note on sizes: the REU and GeoRAM handlers allocate RAM banks when they start
(`NumREU_Banks`, `GeoRAM_Bank[]`), so their real size depends on how much RAM
is free. The `Provides` string holds the size the handler *aims* for. The
resolver should check it against the handler's actual size, using a small
`QueryCaps()` hook per handler where the size can vary, so that it never
promises 512 KB and then delivers 384 KB.

### 1.3 What the system provides

Some capabilities don't come from any handler:

- **From the launch:** `media src=sd|usb|tr`, `machine …`, `tr model=…`. TR
  already knows these.
- **From the user's setup:** `iec …`. TR is a cartridge-port device and can't
  see the IEC bus, so it can't detect drives. The user describes them once in
  the global file (see Part 5):

  ```ini
  [@system]
  provide iec device=8 type=1541 drivecode=1541
  provide iec device=9 type=sd2iec drivecode=none
  ```

  If there is no `[@system]` section, every `iec` question is **unknown**, not
  "absent". How unknown answers are treated is covered in 3.4.

### 1.4 Abstract capabilities

`disk` is defined in firmware as an alias, not provided directly:

```
disk  ≡  vdrive | iec
```

So `need disk` has the same meaning as `need vdrive | iec`. Parameters carry
through: `need disk device=8` is `vdrive device=8 | iec device=8`. Aliases are
fixed in firmware and are not user-definable in v1, which keeps the
language small.

### 1.5 Device-number conflicts

`vdrive device=8` and `iec device=8` can't both answer on device 8. The
resolver treats any two `device=` providers with the same number as a
conflict. A candidate handler that would create one is ruled out *for programs
that use devices* (any `need`/`want` that mentions `disk`, `iec` or `vdrive`).
For other programs the conflict is still flagged, and the message explains it.
VDrive could later take a `device` parameter of its own and move to 9, which
would remove most of these conflicts.

---

## Part 2: The declaration language

### 2.1 Format

There is one INI-style **section for each file or wildcard**, followed by
requirement lines. The section header is everything from the first `[` to the
**last** `]` on the line, so file names containing `]` work.

```ini
# .specialio
[skinnyai.prg]
need swiftlink
want reu
need disk
```

### 2.2 Requirement lines

| Keyword | Meaning |
|---|---|
| `need A \| B \| …` | **Required.** At least one alternative must be met. Several `need` lines must *all* be met. |
| `want A \| B \| …` | **Optional.** Satisfy it if possible. If several `want` lines can't all be met, earlier lines win. |
| `forbid A \| B \| …` | **Must not be present.** If *any* alternative is present, the candidate is ruled out. |
| `avoid A \| B \| …` | **Better not.** Like `forbid`, but only lowers the candidate's score. |
| `handler TOKEN` | **Use this handler** and skip the resolver (see 2.4). |
| `message TEXT` | Text added to any refusal or warning for this file, e.g. "Load from a real 1541". |
| `strict` | Treat *unknown* system capabilities as absent for this file (see 3.4). |

Within one line, alternatives are listed **in order of preference**. `need iec
| vdrive` says both work and a real drive is preferred.

### 2.3 Capability terms

```
term       := name[:value] { param }
param      := key op value
op         := "=" | "!=" | ">=" | "<="
value      := word | number[k|m]          ; 128k, 512k, 1m, 16m
```

- `name:value` is shorthand for the capability's main parameter: `midi:datel` is
  `midi type=datel`, and `iec:8` is `iec device=8`. Each capability defines
  which parameter that is (`type` for most, `device` for `iec`, `vdrive` and
  `disk`, `src` for `media`).
- A term with no parameters matches any variant: `need midi` accepts all four
  MIDI types.
- A parameter that the provider doesn't state **does not match** a `need`, but
  is fine for `want`.
- Tokens and keys are case-insensitive. `#` starts a comment when it begins a
  line, or after whitespace.

### 2.4 Naming a handler directly

`handler vdrive-swift-reu` skips the resolver completely. It is there for odd
software that only works with one exact handler, and for users who want
complete control. It is also the only way to select handlers the resolver
never picks on its own, such as `kernal`. If the handler is not in this build,
the launch is refused with a message.

### 2.5 Forward compatibility

A declaration file can be newer than the firmware reading it:

- An **unknown capability** in `need`: that alternative counts as *not met*. If
  no alternative is left, the launch is refused, and the message names the
  token and suggests a firmware update.
- An unknown capability in `want`, `avoid` or `forbid` is ignored. For
  `forbid`, ignoring it errs toward launching, which matches how unknown tokens
  are handled elsewhere.
- An **unknown parameter key** makes that term not match in `need`, and is
  ignored elsewhere.
- An **unknown keyword** at the start of a line: the line is skipped and the
  serial debug output logs it.

---

## Part 3: How the firmware picks a handler

### 3.1 Inputs

- The declaration for the file, which is the first matching section (Part 6).
- The **candidates**: every handler in this build with `IOHDecl_Allowed`.
- For each candidate, the capabilities it **provides**: its `Provides` list
  (checked against `QueryCaps()`), plus the system capabilities from the launch
  and from `[@system]`.
- The user's **Settings default** (`IO1[rwRegNextIOHndlr]`).

### 3.2 Steps

1. **Filter.** Rule out a candidate if any `need` line has no alternative it
   meets, if any `forbid` alternative is present, or if it creates a device
   conflict (1.5) for a program that uses devices.
2. **Score** each remaining candidate. Compare scores in this order and stop at
   the first difference:
   1. `want` lines met, comparing line by line from the first, so meeting the
      first `want` beats meeting any number of later ones;
   2. fewest `avoid` matches;
   3. the most preferred alternative met on each `need` and `want` line, in
      order;
   4. **the user's Settings default**, if it is still a candidate;
   5. **the fewest extra capabilities**: don't start an REU or a VDrive the
      program didn't ask for, since they use RAM and IO space;
   6. order in `IOHandler[]`.
3. **Result.** Use the highest-scoring candidate for this launch only. If none
   is left, refuse (3.3).

The Settings default comes **after** the requirements and their order of
preference, so a declaration always gets what it asked for. It comes **before**
"fewest extras", so when several handlers qualify equally, the user's own
choice wins. A program with no declaration skips all this and uses the
Settings default, as it does today.

### 3.3 When nothing qualifies

The launch is refused with a short message on the C64:

```
Can't launch globe.prg
 Needs: reu size>=512k
 Best here: vdrive-swift-reu (128k)
[RETURN] menu   [F1] launch anyway
```

The message names the first unmet `need` line, and the closest candidate when
there is one. It adds the `message` text if the section has one. **Launch
anyway** starts the Settings default. A power-up option, kept in the
`RegPowerUpDefaultMasks*` bits, can turn all declarations off for a card with
bad files on it.

### 3.4 Unknown system capabilities

When the user hasn't described their IEC drives, an `iec` check gives
**unknown**. By default:

- in `need`, unknown counts as **met**, and the launch shows a one-line note,
  "assuming an IEC drive on device 8". Most users have a drive, and refusing
  everything until they write a `[@system]` section would be hostile;
- in `forbid`, unknown counts as **not present**;
- in scoring, unknown counts as **met** for `want`, but scores below a known
  provider. So VDrive, which TR knows about for certain, beats an assumed IEC
  drive when the declaration lists them as equal alternatives.

`strict` on a section, or a global `strict` in `[@system]`, makes unknown count
as **absent**.

---

## Part 4: Examples

Every example shows the declaration, then what happens on a few different
setups. Unless stated otherwise, assume a TR+ build that includes the VDrive
handlers, and a Settings default of `none`.

### 4.1 skinnyai.prg: SwiftLink required, REU optional, disk required

> "I need SwiftLink, I'll use an REU if there is one (but I don't need one), and
> I need disk access, which doesn't have to be VDrive, but I won't work
> without either an IEC drive or VDrive."

```ini
[skinnyai.prg]
need swiftlink
want reu
need iec | vdrive          # same as: need disk
```

**Setup A: no `[@system]` section.**

| Candidate | swiftlink | iec \| vdrive | reu (want) | Result |
|---|---|---|---|---|
| `swiftlink` | yes | iec *unknown* → met | no | candidate |
| `vdrive-swift` | yes | vdrive yes | no | candidate |
| `vdrive-swift-reu` | yes | vdrive yes | yes (128k) | candidate |
| others | no | | | ruled out |

`vdrive-swift-reu` is chosen, because it is the only candidate that meets the
`want`. The program gets a 128 KB REU.

**Setup B: `[@system] provide iec device=8 type=1541 drivecode=1541`.** All
three are still candidates, but `vdrive-swift-reu` and `vdrive-swift` put
VDrive on device 8, where the real 1541 is (1.5). They are ruled out, because
the program uses devices. `swiftlink` is chosen: SwiftLink and the real 1541,
with no REU. If VDrive could move to device 9, `vdrive-swift-reu` would be
allowed back in and would win because of the REU.

**Setup C: the current firmware, with no VDrive handlers.** The only candidate
is `swiftlink`. The drive is unknown and counts as met, so it launches with
the note "assuming an IEC drive on device 8".

**Setup D: a TR (not TR+) build without SwiftLink.** No candidate meets `need
swiftlink`, so the launch is refused: "Needs: swiftlink".

**Setup E: Settings default is `swiftlink`, with a real 1541 declared.** This
gives the same result as B. The default wins step 4, but `vdrive-swift-reu` was
already ruled out in step 1.

### 4.2 globe.prg: needs an REU of 512 KB or more

```ini
[globe.prg]
need reu size>=512k
message Needs a 512 KB REU (1750 or larger)
```

| Candidate | reu size | Result |
|---|---|---|
| `reu` | 512k | candidate |
| `vdrive-reu` | 512k | candidate |
| `vdrive-swift-reu` | 128k | **ruled out** |

`reu` and `vdrive-reu` score the same on the requirements. If the Settings
default is `vdrive-reu`, that is chosen (step 4). Otherwise `reu` wins on
fewest extras (step 5), so no VDrive is started.

**If there is too little free RAM:** `QueryCaps()` reports that `reu` can only
allocate 384 KB. Both candidates are ruled out, and the message says
`Best here: reu (384k)`.

**On a non-TR+ build without `Fab04_REU`:** no candidate, so the launch is
refused with the `message` text.

An exact size works too. For a program that detects REU size by wrapping and
breaks on a larger one, write `need reu size=128k`. On the table above only
`vdrive-swift-reu` would qualify, unless the REU handler accepts a size when it
starts (see Questions).

### 4.3 Fastloader that only works with a real 1541: refuse to launch from SD

The program loads its later parts with drive code uploaded to the 1541. VDrive
can't run drive code, and SD2IEC can't either. The boot file happens to be in
an SD collection.

**Variant A: never launch it from SD.** The rest of the program is on a real
disk anyway, so starting the boot file from SD makes no sense:

```ini
[Last Ninja 2 boot.prg]
forbid media:sd | media:usb
message Uses a 1541 fastloader. Load it from the original disk on a real drive.
```

Every launch from SD or USB is refused with that text, whatever the handlers
or `[@system]` say.

**Variant B: launching from SD is fine if a real 1541 with the disk inserted
is on device 8.** The boot file loads from SD, and the fastloader then
continues from the drive:

```ini
[Last Ninja 2 boot.prg]
need iec device=8 drivecode=1541
forbid vdrive:8
forbid kernal              # a replacement KERNAL (e.g. JiffyDOS) can upset some fastloaders
strict                     # don't assume a drive: it must be declared
message Needs a real 1541 (or Pi1541) on device 8 with disk 2 inserted.
```

- With `[@system] provide iec device=8 drivecode=1541`: every candidate
  without VDrive on device 8 or a replacement KERNAL qualifies. Step 5 picks
  `none`, unless the Settings default (for example `swiftlink`) qualifies, in
  which case that is kept.
- With `provide iec device=8 type=sd2iec drivecode=none`: the launch is refused.
  The message says `Needs: iec device=8 drivecode=1541` followed by the
  `message` text.
- With no `[@system]` section: `strict` turns unknown into absent, so the launch
  is refused. Without `strict` it would launch with the note "assuming…".

### 4.4 MIDI sequencer that works with two interfaces

```ini
[Dr.T KCS*.prg]
need midi:passport | midi:datel
```

- Settings default `midi-datel`: both candidates meet the same requirements,
  but Passport is listed first, so step 3 picks `midi-passport`.

  Whether the user's default should win here instead is open (see Questions).
  To make the user's choice win, move step 4 above step 3. This proposal keeps
  the author's preference first, because an author who lists Passport first
  usually means "Passport works better".
- Settings default `swiftlink` (not a candidate): `midi-passport`.

### 4.5 A program that must not have an REU

Some programs use `$DF00` for their own purposes, or check for an REU and then
misbehave:

```ini
[Old demo.prg]
forbid reu | georam
```

With Settings default `reu`, the default is ruled out. Step 5 then picks the
candidate with the fewest extras, which is `none`. With Settings default
`swiftlink`, that is kept, because SwiftLink doesn't use IO2.

### 4.6 Either kind of RAM expansion

```ini
[Ramdisk tool.prg]
need reu size>=256k | georam size>=512k
```

`reu` (512k) and `vdrive-reu` meet the first alternative. `georam` meets the
second, provided it allocated 512 KB. `reu` wins on preference order (step 3).
If REU is not in this build, `georam` is chosen.

### 4.7 A program that uses the USB mouse

```ini
[Paint 64.prg]
want mouse:1351
```

The mouse registers only exist under the GeoRAM handler today, so `georam` is
chosen. The program never asked for GeoRAM, but `mouse` comes with it. This
shows why capabilities are useful: a program asking for a mouse would never
think to name a GeoRAM handler. Because it is only a `want`, the program still
launches on builds without it.

### 4.8 A tool that uses the TR block device

```ini
[geos-disk-tool.prg]
need trblock
need tr model=trplus | tr model=tr
```

The resolver picks `georam`, the handler that carries `trblock`
(`docs/TR_Block_Device.md`).

### 4.9 Several capabilities, all required

```ini
[BBS Server.prg]
need swiftlink
need reu size>=128k
need disk
avoid iec:8                # prefers VDrive, so the real drive is free for the sysop
```

`vdrive-swift-reu` is the only handler that provides SwiftLink, an REU and a
disk together, so it is chosen. If a real 1541 is declared on device 8,
`vdrive-swift-reu` is ruled out by the conflict (1.5), and no candidate is
left. The message then says: `VDrive and IEC drive both on device 8`.

### 4.10 A whole folder of demos

```ini
# .specialio in /Demos/REU/
[*.prg]
want reu size>=512k
```

Every PRG in the folder gets the biggest REU available, and nothing is refused.
A file listed in its own section further up the file overrides this, because
the first match wins (Part 6).

### 4.11 Naming a handler directly

```ini
[Weird Test.prg]
handler vdrive-swift-reu
```

This runs on exactly that handler and is refused on any build without it.

### 4.12 A user override in the global file

```ini
# /specialio.txt
[/Terminals/CCGMS 2021.prg]
need swiftlink             # the folder's file also asks for an REU and a disk; I don't care

[/Demos/REU/broken one.prg]
handler none

[/Games/*]
default                    # ignore all folder declarations under /Games, use Settings
```

`default` is a section-level keyword. It stops the search and uses the
Settings default, without refusing.

### 4.13 A newer declaration on older firmware

```ini
[Net Game.prg]
need ethernet-socket       # capability added in a later firmware
want reu
```

Older firmware doesn't know `ethernet-socket`, so the `need` line has no
alternative it can meet, and the launch is refused: `Needs "ethernet-socket":
not supported by this firmware - update?`.

---

## Part 5: Where declarations live

Moving from handler names to capabilities changes the *format* of the files,
not where they live. The location options from revision 1 still apply.

### Option A: a hidden file in each folder (`.specialio`) *(recommended)*

The file covers the files in its folder. It travels with a collection when the
folder is copied. `LoadDirectory()` already hides names that start with `.`
(`IsDotfile()` in `DriveDirLoad.h`). Reading the file adds about one file open
per directory load. The file is read with a size limit of about 4 KB, line by
line, from a stack buffer, with no heap.

The drawback is that macOS Finder hides dotfiles, so Mac users can't easily
see or edit the file. AppleDouble twins (`._.specialio`) are ignored, because
the parser opens the exact name only.

### Option C: one global file at the card root (`/specialio.txt`) *(recommended)*

This file sits next to `autolaunch.txt` and is visible. Section headers are
**paths from the drive root**, and wildcards are allowed. It is also the only
place for `[@system]` (1.3): a folder's file can't declare what hardware the
user has.

### Option D: a hash database *(possible later stage)*

This works like HVSC's `Songlengths.md5`, but maps file hashes to *declaration
bodies*, so it still works when files are renamed. The capability language
makes it practical, because a database entry like `need reu size>=512k` means
the same on every build.

### Options not recommended

- **Option B, a sidecar file for each PRG:** files multiply, and a renamed PRG
  leaves its sidecar behind. Option A can already hold a single entry.
- **Option E, magic bytes in the PRG:** every other loader (a real 1541, SD2IEC,
  Ultimate, VICE) would load the extra bytes into RAM. It also changes the
  file's hash, the user can't change it, and old software, which is most of the
  cases this is for, would have to be patched. A **runtime request** from a
  TR-aware PRG through IO1, in the same way `rsIOHWSelInit` already switches
  handlers, is still a clean option later. It would pass a declaration string
  in this same language.
- **Option F, tags in file names (`[SwiftLink]`):** this can't express
  `want`, alternatives or parameters.

---

## Part 6: Precedence between files

1. The user's choice for this launch (a future "launch with…" key).
2. **The global `/specialio.txt`**: the first section whose path matches.
3. **The folder's `.specialio`**: the first section whose name matches.
4. *(later)* The hash database.
5. **No declaration**: the Settings default, as today.

A matching section is used **whole**. Sections from different levels are not
merged, so what the user gets is exactly what is written in one place. To
build on a folder's declaration, the user copies it into the global file and
edits it (4.12). Merging could be added later, but it makes the result much
harder to predict.

`[@system]` is used whatever the level, because it describes the setup, not a
file.

---

## Part 7: Creating and installing declarations

| Who | How |
|-----|-----|
| Collection authors | Ship a `.specialio` in each folder. |
| Users on a PC | Edit `/specialio.txt`, including `[@system]`. Document the format in `docs/specialio.txt`, in the same way as `docs/autolaunch.txt`. |
| Users on the C64 | (1) **Settings → My drives**: a short form that writes `[@system]` (device 8–11: none / 1541 / 1571 / 1581 / SD2IEC / other). (2) A file-browser action, **"Requirements…"**, next to *Set REU File* / *Set KERNAL file* (`StatusFunctions.c`). It shows the section in effect and what it resolves to, for example `-> vdrive-swift-reu (reu 128k)`. A simple editor for `need`/`want` covering common capabilities is a possible later addition. |
| Host apps (TeensyROM-UI, CLI) | Write the files over the existing file-transfer commands. A new remote command, *resolve path*, returns the chosen handler and the reasons, so a host app can show "this will run with VDrive+Swift+REU (128 KB REU)" or "can't run: needs REU ≥ 512 KB" before the user launches. |
| VDrive and Host Extensions | Handlers that come from an extension declare `Token` and `Provides` in their manifest, so `VmRegistry` can add them to the resolver's candidates without changing the firmware. |

---

## Part 8: Implementation outline

1. **Tokens and provides.** Add `Token`, `Provides` and `DeclFlags` to
   `stcIOHandlers`, and an optional `QueryCaps()` for handlers whose size can
   vary. Update the `add-io-handler` checklist: a new handler needs all three.
2. **Parser.** Turn a section into a small fixed-size structure: at most 8
   lines, 4 alternatives per line and 3 parameters per term, all as tokens and
   integers, with no heap. Parse the `Provides` strings with the same code.
   Add host-side unit tests under `Source/Teensy/tests/` alongside the existing
   harnesses, covering the grammar, wildcards, `]` in names, CRLF line endings,
   unknown tokens, the size limit, and the size suffixes `k` and `m`.
3. **Resolver.** `ResolveIOH(const Decl&, LaunchCtx&) -> {handler, reason}`, the
   algorithm in Part 3. It is a pure function, so it can be unit-tested on the
   host against the examples in Part 4. **Part 4 becomes the test cases.**
4. **Always initialise `IOHndlrAssoc`** for drive entries
   (`SetDriveDirMenuNameType`, `AddDirEntry`) to a value meaning "no
   declaration". This fixes the uninitialised-memory issue and is worth doing
   regardless.
5. **When resolution happens.**
   - `LoadDirectory()` reads both files once per directory and resolves each PRG
     entry. It stores the handler in `IOHndlrAssoc`, or a "refused" marker, so
     the listing can show an icon for both.
   - `RemoteLaunch()` builds its one-item menu without calling
     `LoadDirectory()` (`RemoteControl.ino`, around line 276), so it calls the
     resolver itself. This covers autolaunch, hot keys, NFC and host-app
     launches.
   - The decision is checked again at launch, because `QueryCaps()` sizes can
     change between the directory load and the launch.
6. **Remove the `rmtTeensy` check** in the three places it appears, and replace
   them with `SelectedItemIOH()`.
7. **Refusal screen and "launch anyway"** (3.3), plus the power-up option to
   turn declarations off.
8. **The built-in TR menu:** `IOHndlrAssoc` in `MainMenuItems.h` stays as it is,
   effectively a fixed `handler` line. TR menu items could later use
   declarations too.

---

## Questions to decide

1. **Settings default against the author's preference** (4.4): should the
   user's default beat the order of alternatives (step 4 above step 3)?
2. **Unknown IEC** (3.4): should the default be "assume present" (as
   proposed), or strict?
3. **Device conflict** (1.5): rule out the candidate (as proposed), or allow it
   with a warning? And should VDrive take a `device` parameter so it can move
   to device 9?
4. **REU size when starting:** should the REU handler accept a requested size
   (`reu size=128k` gives exactly 128 KB), so a single `reu` handler can serve
   exact-size requests? Then `Provides` would list a range, such as
   `reu size=128k..512k`.
5. **Merging levels** (Part 6): keep "the first matching section is used whole"
   (as proposed), or let the global file add to a folder's section?
6. **File names:** `.specialio` and `/specialio.txt`, or put everything under a
   `/.teensyrom/` folder?
7. **Token spellings** for capabilities and handlers: settle them before
   release, because they are permanent once released.
