# Proposal: Special IO Declarations for PRGs on SD/USB

Status: **Draft for discussion**

## Problem

Some PRGs only work with a particular Special IO handler: terminal programs need
SwiftLink, MIDI sequencers need a Datel, Sequential, Passport or Namesoft
interface, and some games and demos need an REU or GeoRAM. Today the user picks
the handler once from **Settings → Special IO**. That choice is stored in EEPROM
(`eepAdNextIOHndlr`) and applies to *every* PRG launched afterwards. So
launching a SwiftLink terminal and then a MIDI program means going back to
Settings in between, and a PRG run with the wrong handler fails without saying why.

The firmware already has a per-item mechanism, but it covers only the built-in
TR menu:

- `StructMenuItem.IOHndlrAssoc` (`Menu_Regs.h`) holds a handler for each menu item.
- `IOHandlerSelectInit()` (`IOHandlers.ino`), `InitHndlr_KERNALReplace_PreStart`
  logic (`StatusFunctions.c`) and `UpdateItemTypeReg()` (`IOH_TeensyROM.c`, which
  sets bit 7 to show an assigned handler) all honour it.
- All three check `IO1[rWRegCurrMenuWAIT] == rmtTeensy` first. This is needed:
  `DriveDirMenu` entries are `malloc`ed in `LoadDirectory()` and
  `SetDriveDirMenuNameType()`, and nothing initialises their `IOHndlrAssoc`.

This proposal covers how a PRG on SD or USB can declare the handler it needs, so
that the same mechanism works for media too.

## Goals

1. A **stable identifier** for each Special IO that does not change between
   firmware builds or releases.
2. A declaration that is **easy to write and to share**, and that travels with a
   collection of files when it is copied to a card.
3. A clear **precedence** between what a collection's author declares and what
   the user wants.
4. Every launch path gives the same answer: browsing the menu, autolaunch, hot
   keys, NFC tags, and remote launch from host apps.
5. No changes to the PRG files themselves (see Option E for why).

Not in scope: CRTs. Their hardware type is already in the CRT header. The one
exception is that a generic CRT inherits Kernal Replace (see `IOH_KernalReplace.c`).
That behaviour is left unchanged.

---

## Part 1: Stable identifiers for each Special IO

### The numeric enum can't be used

`enumIOHandlers` is not stable:

- Its values shift with build flags. `MinimumBuild` drops every full-build entry,
  and `Fab04_REU` and `Fab04_KernalReplace` add entries in the *middle* of the
  selectable range. `IOH_Debug` has a different number with and without REU.
- Adding a handler anywhere except the end renumbers everything after it. (The
  `IOHandlerInit()` comment about "an EEPROM byte written by a build with more
  handlers" is this problem already happening with `eepAdNextIOHndlr`.)

### The display name shouldn't be used either

`stcIOHandlers.Name` (for example `"MIDI:Datel/Siel"`) is UI text. It is limited
to 20 characters by the C64 screen layout, and it is reasonable to reword it.

### Proposal: a token for each handler

Add a short, lower-case token to each handler, defined next to the handler
itself so the two can't get out of sync:

```c
struct stcIOHandlers
{
  char Name[IOHNameLength];   // display name, may change
  const char *Token;          // stable ID used in config files, NEVER changes once released
  uint8_t DeclFlags;          // e.g. IOHDecl_Allowed: may be named by a declaration
  void (*InitHndlr)();
  ...
};
```

Proposed tokens:

| Token          | Handler              | Notes                         |
|----------------|----------------------|-------------------------------|
| `none`         | IOH_None             | explicitly "no Special IO"    |
| `default`      | (keyword, not a handler) | "use whatever Settings says" |
| `swiftlink`    | IOH_Swiftlink        |                               |
| `midi-datel`   | IOH_MIDI_Datel       |                               |
| `midi-seq`     | IOH_MIDI_Sequential  |                               |
| `midi-passport`| IOH_MIDI_Passport    |                               |
| `midi-namesoft`| IOH_MIDI_NamesoftIRQ |                               |
| `reu`          | IOH_REU              | TR+ / `Fab04_REU` builds only |
| `kernal`       | IOH_KernalReplace    | `Fab04_KernalReplace` only    |
| `georam`       | IOH_GeoRAM           |                               |
| `debug`        | IOH_Debug            | probably not `IOHDecl_Allowed`|
| `tr-basic`     | IOH_TR_BASIC         | candidate; not selectable today |

Rules:

- A token is **never reused or renamed** once released. To retire one, keep it
  and map it to its replacement or to `none`.
- Lookup is a linear `strcasecmp` over `IOHandler[]`. With about 30 entries that
  happens once per directory load, so it costs nothing worth measuring.
- A token the running build doesn't know about (an REU file on a non-TR+ build,
  a newer token on older firmware) resolves to **"unavailable"**, not to
  `IOH_None` without warning. The launch then shows a one-line message, for
  example `Needs Special IO "reu" - not available on this build`, and continues
  with `default`. This follows the `TRPlusOnlyMsg()` pattern.
- `IOHDecl_Allowed` is a separate flag from `LastSelectableIOH`. That way handlers
  such as `tr-basic` can be allowed later without making them selectable in
  Settings, and freezers and `TeensyROM` itself can never be named.
- Update the `add-io-handler` checklist: a new handler needs a token.

Later, the same tokens could also be used in EEPROM (`eepAdNextIOHndlr`), in the
remote-control protocol, and in host apps. That would fix the existing
renumbering hazard, but it is a separate change.

### Handlers that take parameters

Some handlers depend on other settings: REU and Kernal Replace use a file path
(`eepAdREUFilename`, `eepAdKERNALBinName`), and REU size, GeoRAM size and MIDI
settings live in registers and EEPROM. The proposed syntax reserves space for
`key=value` options after the token, for example `reu size=512k file=game.reu`.
**Version 1 would accept and ignore them**, so files written for later firmware
don't fail to parse.

---

## Part 2: Where declarations live

Four places are possible. They can be combined, and the recommendation below
uses two of them.

### Option A: one hidden file per directory (`.specialio`) *(recommended)*

There is one file in each directory, covering the files in that directory:

```ini
# .specialio  -- TeensyROM Special IO declarations for this folder
# [token] header, then one file name (or glob) per line. Case-insensitive.

[swiftlink]
CCGMS 2021.prg
Novaterm 9.6*.prg

[midi-datel]
Dr.T KCS*.prg

[reu]
Sonic the Hedgehog.prg
```

The format uses a section for each handler and **one file name per line**, so
file names can contain any character, including spaces, `=` and `#` (`#` starts
a comment only at the beginning of a line). This avoids the quoting problems of
a `name = value` format.

Pros:
- It travels with the folder. A collection published as a zip, such as a
  "Terminals" or "MIDI" folder, includes its `.specialio` and works as soon as
  it's copied.
- The directory listing hides it already, because `LoadDirectory()` skips names
  that start with `.` (`IsDotfile()` in `DriveDirLoad.h`).
- It's cheap. `LoadDirectory()` already opens the directory, and reading one
  small file there (with a size limit of about 4 KB, parsed line by line from a
  stack buffer, no heap) adds about one extra file open.
- The `[ ]` section syntax is easy to edit by hand and to generate with tools.

Cons:
- macOS Finder hides dotfiles, so Mac users can't easily see or edit the file.
  (Windows and Linux file managers show them or have a toggle.) A visible
  alias, `specialio.txt`, could also be accepted, but then it would appear in
  the C64 listing as an unknown file. That is solvable (skip it by name), but
  it is one more special case.
- Renaming a PRG on a PC breaks its declaration without any warning. Globs
  (`Novaterm*.prg`) make this less likely.
- AppleDouble twins (`._.specialio`) are ignored, because the parser opens the
  exact name only.

### Option B: a sidecar file for each PRG (`.Game.prg.sio` or `Game.prg.sio`)

Pros: the declaration sits right next to the file it describes, and is easy to
add or remove one file at a time.
Cons: one extra file per PRG. A renamed PRG leaves its sidecar behind. Each
launch needs another file open, or the whole directory listing has to be
matched. A visible sidecar clutters the C64 listing, and a hidden one has the
macOS problem from Option A.
Verdict: no clear advantage over Option A, which can already hold a single
entry. Not recommended.

### Option C: one global file at the card root *(recommended, as the user override)*

`/specialio.txt` on SD and on USB, in the same place as `autolaunch.txt` for
consistency. It is visible, so Mac users can edit it. The format is the same,
except that entries are **paths from the root of that drive**, and globs and
folders are allowed:

```ini
# /specialio.txt  -- user overrides, beats any per-folder .specialio

[swiftlink]
/Terminals/*                    # everything in a folder
/Utils/My Own Term.prg

[none]
/Terminals/Readme Viewer.prg    # override the folder's own declaration

[default]
/Demos/Weird REU demo.prg       # ignore any declaration, use Settings
```

Pros: one place to change for "my setup". It is useful when users reorganise
files from different collections, or want to override a collection's
declaration. It also suits a user who doesn't want a dotfile in every folder.
Cons: a global file goes stale when files are moved or renamed. It could also
grow large; with a 4 to 8 KB limit, reading it costs little.

### Option D: a hash database (`/.teensyrom/specialio.db`) *(possible later stage)*

This works like HVSC's `Songlengths.md5`: entries are keyed by a hash of the
file contents, not by name.

```
; md5 of file                       token
9b1e0c...e7  swiftlink   ; CCGMS 2021
```

Pros: it still works when files are renamed or moved, and the community could
maintain it centrally, sorted by CSDb release. A host app (TeensyROM-UI) could
ship it and update it.
Cons: every PRG launch has to be hashed. That is cheap, since the whole PRG is
already in RAM (≤ 64 KB) before the transfer, but it can only happen **at
launch time**. The listing, which today shows bit 7 in `rRegItemTypePlusIOH`,
can't show the assignment in advance. Someone also has to build and maintain
the database.
Verdict: worth keeping in mind. The token scheme from Part 1 is what makes it
possible.

### Option E: magic bytes inside the PRG *(not recommended)*

Examples: a trailer after the program data with a signature such as
`"TRSIO" + token + checksum`, or a header in a dummy segment.

Problems:
- **Every other loader will load the extra bytes.** A real 1541, SD2IEC,
  Ultimate, VICE or a cartridge fastloader loads the trailer into RAM after the
  end of the program, where it could overwrite data the program expects to be
  zero or its own variables. A header is worse, because it moves the load address.
- It changes the file's checksum and contents. CSDb and preservation hashes,
  and Option D, no longer match the original release.
- The user can't change it without modifying the binary.
- A short signature can match by chance, which would activate a handler for a
  random PRG.
- It only helps software written for TeensyROM. Older terminal and MIDI
  software, which is most of the cases this proposal is for, would have to be
  patched.

A related option is a **runtime request**: a TeensyROM-aware PRG asks for a
handler through an IO1 register handshake after it starts, in the same way
`rsIOHWSelInit` already switches handlers after a PRG transfer. This is clean
for new software and changes nothing on disk, so it could be added later. It
doesn't help existing software, though, so it doesn't replace the file options.

### Option F: a tag in the file name (`CCGMS [SwiftLink].prg`)

This follows the custom of tagging CRT file names with `[EasyFlash]`. Pros: it
can be seen everywhere and needs no extra files. Cons: it makes users rename
their files, it is long for the C64 menu's 38-character column, it conflicts
with other naming conventions (OneLoad64 and others), and parsing it is unclear
(`[REU]` is also used as a plain descriptive tag). It could be accepted as a
fallback with the lowest priority, but should not be the main mechanism.

---

## Part 3: Precedence

The recommended order, from highest priority to lowest:

1. **The user's choice for this launch.** For example, a future "launch with…"
   hot key in the file browser.
2. **The global user file**, `/specialio.txt` (Option C). The user's own setup
   beats what a collection author wrote. This is the "global file at the top
   that overrides individual declarations" idea.
3. **The folder's `.specialio`** (Option A). This is the author's or the
   collection's declaration.
4. *(later)* **The hash database** (Option D).
5. **The Settings → Special IO default** (`IO1[rwRegNextIOHndlr]`), which is
   what happens today.

Notes:
- Within one file, **the first match wins** in file order, so an exact name
  listed above a glob takes priority over the glob.
- `[default]` means "stop looking and use level 5". It is how the user (level 2)
  turns off a folder's declaration without picking a different handler.
  `[none]` means "run with no Special IO, whatever Settings says".
- A declaration applies **to that one launch only**. It doesn't change the
  EEPROM default, which matches how `IOHndlrAssoc` works for the TR menu. When
  the user exits to the menu, Settings applies again.
- A power-up option to ignore all declarations, kept in the
  `RegPowerUpDefaultMasks*` bits, gives users an easy way out if a card has bad
  files on it.

---

## Part 4: Creating and installing declarations

| Who | How |
|-----|-----|
| Collection authors | Put a `.specialio` in the folder and ship it in the zip. |
| Users on a PC | Edit `/specialio.txt` in a text editor, next to `autolaunch.txt`. Document the format in `docs/specialio.txt`, like `docs/autolaunch.txt`. |
| Users on the C64 | Add a file-browser action, **"Set Special IO for this file"**, next to the existing *Set REU File* / *Set KERNAL file* actions in `StatusFunctions.c`. It cycles through the allowed tokens, as the Settings selector does, and writes or updates the entry in that folder's `.specialio`, or in `/specialio.txt` if the user prefers, which could be a setting. |
| Host apps (TeensyROM-UI, CLI) | Write the files over the existing file-transfer commands. An optional later command could return the resolved handler for a path, so the app can show it. |

Writing to the files needs SD or USB write support, which the HTTP-download and
REU-save features already use.

---

## Part 5: Implementation outline (Options A + C)

1. **Tokens.** Add `Token` and `DeclFlags` to `stcIOHandlers`, fill them in for
   every handler, and add `IOHFromToken(const char*)`, which returns the enum
   value, `IOH_Decl_Default`, or `IOH_Decl_Unavailable`.
2. **Always initialise `IOHndlrAssoc`** for drive entries
   (`SetDriveDirMenuNameType`, `AddDirEntry`) to a sentinel meaning "no
   declaration". This fixes the uninitialised-memory issue described above and
   is worth doing regardless.
3. **One resolver**, `ResolveDeclaredIOH(FS*, const char *Dir, const char
   *FileName)`. It reads the global file, then the folder file, in a single pass
   over each, and returns the handler.
   - `LoadDirectory()` calls it once per directory, not once per file: it reads
     both files and sets `IOHndlrAssoc` for each PRG entry.
   - `RemoteLaunch()` builds a one-item `DriveDirMenu` *without* calling
     `LoadDirectory()` (see `RemoteControl.ino` around line 276), so it calls the
     resolver for that one item. This also covers autolaunch, hot keys, NFC
     (`EEPRemoteLaunch`) and host-app launches.
4. **Remove the `rmtTeensy` check** in the three places it appears, and replace
   them with a helper, `SelectedItemIOH()`. The listing's bit 7 indicator then
   works for SD and USB with no further changes.
5. **Messages.** If a declared handler is unavailable, show one line and
   continue with the default.
6. **Restrictions.** Declarations apply only to `rtFilePrg` items for now. P00
   and PRGs inside D64 images (`D64.ino`) could later use a `image.d64/FILENAME`
   path form.
7. **Tests.** Add host-side unit tests for the parser under
   `Source/Teensy/tests/`, alongside the existing harnesses: sections, globs,
   comments, CRLF line endings, long names, unknown tokens, the size limit, and
   precedence.

---

## Questions to decide

1. **File names:** `.specialio` and `/specialio.txt`, or put everything under a
   `/.teensyrom/` folder? The proposal uses the `autolaunch.txt` precedent for
   the global file.
2. **Should the global file override the folder file, or the other way round?**
   The proposal puts the user's global file first, because the user owns the
   card.
3. **Visible alternative to the per-folder file:** should a visible
   `specialio.txt` *inside a folder* also be accepted, for Mac users, and hidden
   from the listing by name?
4. **Unavailable handler:** warn and continue (as proposed), or refuse to launch?
5. **Parameters:** are REU size and REU file needed in version 1, or later?
6. **Token names:** settle the exact spellings before release, because they
   are permanent once released.
