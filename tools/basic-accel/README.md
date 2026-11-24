# BASIC float acceleration

An experiment in answering BASIC V2's slow float routines from the cartridge, transparently to
the C64 program. So far it covers:

- the four arithmetic routines (add, subtract, multiply and divide), from zero page or with an
  operand in memory;
- FIN, which parses a number from text;
- QINT and AYINT, which convert FAC to an integer.

Nothing here is in the firmware yet. This is the C ports, the evidence that they are
bit-exact, and a model of what they would gain. [Running the ROM on the Teensy](#running-the-rom-on-the-teensy)
is a second design built on the same ports.

| File | What it is |
|---|---|
| `Source/Teensy/BasicAccel/CbmFloat.h` | the routines in C, bit-exact with the ROM |
| `Source/Teensy/tests/cbm-float.test.js` | checks them against the ROM (`npm test`) |
| `Source/Teensy/tests/cbm-float-vectors.txt` | the ROM's results, so CI checks them without the ROM |
| `tools/basic-accel/float-cases.mjs` | the test cases, and the ROM routines run in `tools/lib/mos6502.mjs` |
| `tools/basic-accel/gen-float-vectors.mjs` | rewrites the vectors file from the ROM |
| `Source/C64/FMultBench/` | C64 benchmark: 4,500 FMULTs, timed with CIA2 |
| `tools/basic-accel/SinePlot.bas` | a type-in style hi-res sine plot in BASIC |
| `tools/basic-accel/vice-float.mjs` | runs either one in VICE, natively or with a modeled cartridge, optionally profiling |
| `Source/Teensy/BasicAccel/Cpu6502.h` | a 6502 core, for running the ROM on the Teensy |
| `Source/Teensy/BasicAccel/BasicEmu.h` | the C64 around it: a copy of RAM, banking, the bus, the hooks |
| `tools/basic-accel/emu-model.mjs`, `emu_model.c` | runs a listing on `BasicEmu.h` against a model of the C64, and times it |
| `tools/basic-accel/basic-tokenize.mjs` | a listing to a `.prg`, as the C64 crunches it |

## Where the hooks go

| Routine | Hooked at | Computes | Callers that meet there |
|---|---|---|---|
| FADDT | `$B86A` | ARG + FAC | `+`; FADD (`$B867`) after CONUPK; FIN's digit loop (`$BD8E`) |
| FSUBT | `$B853` | ARG − FAC | `-`; FSUB (`$B850`) after CONUPK; SIN and others |
| FMULTT | `$BA2B` | ARG × FAC | `*`; FMULT (`$BA28`) after CONUPK |
| FDIVT | `$BB12` | ARG ÷ FAC | `/`; FDIV (`$BB0F`) after CONUPK; DIV10 |
| FADD, FSUB, FMULT, FDIV | `$B867`, `$B850`, `$BA28`, `$BB0F` | as above, with ARG first unpacked from A/Y by CONUPK | POLY, SIN, LOG, EXP and the rest of the ROM's math |
| FIN | `$BCF3` | the number at TXTPTR | a number in program text (`$AE8F`, `$AEAD`); INPUT and READ (`$AC89`); VAL (`$B7DA`) |
| QINT | `$BC9B` | FAC's integer part, 32-bit in `$62-$65` | INT, GETADR (POKE/PEEK addresses), AYINT |
| AYINT | `$B1BF` | the same, for -32768 to 32767 | AND, OR, NOT, integer variables and array indexes |

At each T entry (FADDT and the rest), every input is in zero page (`$61-$70`), and every caller
leaves N/Z set from `$61`. Every caller of these entries in the BASIC and KERNAL ROMs was
checked for that. A cartridge can read memory with DMA. It cannot see A, X, Y or the flags. So
each port reports which registers and flags it determined (`Set`). The test checks that the ROM
leaves every other one as the caller had it.

The ports hand back to the ROM (`CbmFloat_Native`) only when the ROM would raise an error:
`?OVERFLOW ERROR`, `?DIVISION BY ZERO ERROR` or (AYINT) `?ILLEGAL QUANTITY ERROR`.

The arithmetic writes `$26-$2A`, `$56` and `$61-$70`. `$2A` is FDIVT's: its last quotient
store, `STA $29,X` with X = 1, lands one byte past the product.

### The operand entries: CONUPK folded in

FADD, FSUB, FMULT and FDIV take their operand's address in A/Y, which a cartridge cannot see.
It serves `STA $DE00 / STY $DE01` first (8 cycles) to learn them, then reads the operand's five
bytes, as the CPU sees them (a ROM constant reads as the ROM). Hooking these entries saves
CONUPK's 60 or so cycles. It also catches the calls POLY, SIN and the like make, which
otherwise reach the T entries only after CONUPK has run natively.

The operand can be in zero page: in BASIC's temporaries, or in FAC or ARG themselves. CONUPK
then reads some bytes it has just written. So the port reads operand bytes below `$100` from
the zero page as it stands at that moment, and the cartridge's five bytes only for the rest
(`CbmOperand`). The tests point operands into FAC, into ARG, and across the end of zero page.
These entries also write `$22-$23`, CONUPK's pointer.

### QINT and AYINT

QINT works in zero page only, with no register inputs. AYINT is QINT behind a range check.
FAC from `$90` up must be exactly -32768, which AYINT checks by FCOMP against its constant at
`$B1A5` (`90 80 00 00 00`), so FCOMP is ported too. The cartridge holds that constant itself.
They write `$62-$65`, `$68` and `$70`; AYINT's FCOMP also writes `$24-$25`. QINT's byte shifts
fill from `$68`: it stores `$FF` there first for a negative number, and the last shift count
after.

### FIN

FIN reads the text at TXTPTR (`$7A/$7B`) through CHRGET, and starts from the A and carry that
CHRGET left. Every caller arrives straight from CHRGET or CHRGOT, so both follow from the byte
at TXTPTR. (`$AEAD` arrives by a `CMP #'.'`, which leaves carry as CHRGET would.)

CHRGET is code in zero page (`$73-$8A`), and wedges patch it. The port runs its own copy, and
hands back to the ROM if the C64's is not the ROM's.

The cartridge passes FIN a window of the text from TXTPTR (`CbmText`). If FIN reads past the
end of the window, the port answers `CbmFloat_MoreText` rather than guess, and the cartridge
reads more and runs it again. Numbers in the sine plot read at most 30 bytes.

FIN reads `$68` and `$73-$8A`, besides the text. It writes `$22-$23` (DIV10's pointer to the
constant 10), `$26-$2A`, `$56`, `$5D-$70` and TXTPTR. On its way it runs MUL10 (which enters
FADDT at `$B877`, below the hook), DIV10, MOVAF and FDIVT. Their ROM routines are ported along
with it.

## Bit-exact includes the ROM's quirks

The ports follow the ROM instruction by instruction, flags included. The one exception is
FMULTT's shift-and-add loop, which is computed as five multiplies. Two of the ROM's quirks
matter:

- **MULSHF's extra signed shift.** If MULSHF (`$B983`) is entered with carry clear, it shifts
  the partial product an extra bit right, as a signed value. In FMULTT that happens for a
  second zero multiplier byte in a row. Leaving it out changes 17% of random products.
- **Fills from `$68`.** An add that shifts an operand by whole bytes fills them from `$68`,
  not with zeros.

The test runs 10,000 cases per routine against the ROM and compares every zero-page byte,
register and flag. The vectors file holds the ROM's results for 20,000 more per routine. A
separate run of 50,000 cases per routine also matched. FIN's cases are number-like text:
signs, spaces, points, exponents signed by characters or by the `+`/`-` tokens, over-long
exponents, and assorted terminators. In VICE, the benchmark's checksum and the sine plot's
bitmap are identical with and without the hooks.

## The VICE model

VICE has no TeensyROM, so `vice-float.mjs` uses VICE's binary monitor to stand in for the
cartridge:

1. An exec checkpoint stops the C64 at a routine's entry.
2. The script reads the zero page and the registers, runs the C port, and writes the result back.
3. It points the PC at a run of NOPs ending in RTS. The run takes as many cycles as the modeled
   cartridge would hold the C64 for.

The C64's own clock then measures the modeled run. While the monitor holds the C64, the C64
does not run, so the script's own time costs it nothing.

The cycle costs are a model (`COSTS` in the script), not a measurement:

| Model | Per call | Made of |
|---|---|---|
| `tr-plus` | ~116–147 | JMP into IO1 (3), thread hand-off (75), /DMA read of what the routine reads (16 for a T entry; 21 and a served `STA/STY` of A/Y for an operand entry; for FIN, `$68`, CHRGET and the text it used) and write-back of the changed bytes, bus turn (4), served register loads and RTS |
| `tr` | ~225–340 | no DMA: the CPU moves the bytes from served code, `LDA zp / STA $DExx` (7 per byte) and `LDA # / STA zp` (5 per byte) |
| `tr-plus-isr` | ~42–71 | `tr-plus` with no hand-off to thread mode: the port runs inside the PHI2 interrupt while the C64 is held |

The hand-off figure is the 70–80 cycles that [Constraints.md](../../docs/Architecture/Constraints.md)
gives for a WriteC64Spans slice. `tr-plus-isr` breaks the rule against new work on the PHI2
path, so it is a bound, not a design. Only a board can say what any of this costs.

VICE is started held at reset (`-initbreak reset`) until the checkpoints are installed. Without
that, the program is autostarted in warp mode while the script is still connecting, and its
first calls run unhooked. The benchmark run reports any FMULT calls it missed.

## Results (VICE 3.7.1, x64sc, PAL)

FMultBench, 4,500 calls, hooked at FMULT (`$BA28`), so CONUPK is part of what the cartridge
answers.

| Model | FMULT cycles/call | Speedup on FMULT | Speedup on the whole pass |
|---|---|---|---|
| native | 2026 | 1.0× | 1.0× (12.26 s) |
| `tr-plus` | 153 | 13.2× | 3.3× |
| `tr` | 362 | 5.6× | 2.6× |
| `tr-plus-isr` | 73 | 28.0× | 3.7× |

SinePlot.bas, plotting 320 points, with every routine hooked. The plot is the same bitmap in
every run.

| Model | Plot time | Speedup |
|---|---|---|
| native | 38.5 s | 1.0× |
| `tr-plus` | 11.9 s | 3.24× |
| `tr` | 16.3 s | 2.36× |
| `tr-plus-isr` | 9.7 s | 3.97× |

The same plot on `tr-plus`, adding one routine at a time:

| Hooked | Plot time |
|---|---|
| none | 38.5 s |
| FMULT | 26.8 s |
| + FDIV | 21.7 s |
| + FADD | 20.4 s |
| + FSUB | 20.4 s |
| + FIN | 13.8 s |
| + FADD, FSUB, FMULT, FDIV (CONUPK folded in) | 12.8 s |
| + QINT, AYINT | 11.9 s |

FADD is called most often but gains least. It is short natively, so a 116-cycle hand-off
leaves little of the saving. With a cheaper hand-off it gains much more. FIN gains most: every
constant in the program (`8192`, `320`, `50.93`) is parsed digit by digit, a MUL10 and an add
per digit, each time its line runs. Hooked, FIN's calls take about 18,000 adds and the DIV10
divides with them.

## What is left

`--profile` samples the C64 at every KERNAL IRQ (50 a second on PAL). It records the routine
that was running and the routines that called it, from the JSR return addresses on the stack.
The sine plot on `tr-plus` with every routine hooked, as a share of the remaining 11.9 s:

| Self time | Routine | What it is |
|---|---|---|
| 30% | (cartridge) | the modeled hand-offs themselves |
| 17% | FRMEVL | the expression evaluator |
| 10% | CHRGET | the interpreter reading program text, in RAM |
| 8% | FADD region | NORMLZ and FADDT's body, entered below the hooks, by INT (`$BCE6`) among others |
| 3% | ORDVAR | variable lookup |
| 3% | PTRGET | variable lookup |
| 3% | EVAL | the expression evaluator |

The candidates, roughly in order:

1. **A cheaper hand-off.** At 30% it is the largest single item, and every further hook pays
   it. On `tr-plus-isr` the same plot takes 9.7 s.
2. **INT** (`$BCCC`). It calls the hooked QINT, then normalizes natively through `$B8D2`.
3. **MOVFM, MOV2F, MOVAF.** They are short. They only pay off with a cheap hand-off, or merged
   into a neighboring hook.

The interpreter itself (FRMEVL, CHRGET, EVAL, PTRGET/ORDVAR, about 36% of what is left) does
not break into routines a hook could take.

## On hardware: the hand-off test

Every cost above rests on a modeled hand-off. `Source/C64/FloatHandoff` measures it on a TR+
through the Debug handler (`IOH_Debug.c`, "float hand-off test"). Nothing there hooks the ROM.
The C64 program asks for each answer itself, by writing `$DEF0`:

| `$DEF0` command | What the TR does |
|---|---|
| 1, ping polled | thread mode sets the status while the C64 polls for it; no hold |
| 2, ping held | holds the C64 by DMA from the ISR (as the REU does), takes it up in thread mode, lets it go |
| 3, routine `$DEF1` | holds the C64, reads `$22-$2A` and `$56-$70` by DMA, runs the port, writes back the spans that changed, lets it go |

`$DEF0` reads back the status. `$DEF2-$DEF7` hold the result's A, X, Y, P and Set.
`$DEF8-$DEFD` hold three TR-side times in nS: from the command write to thread mode taking
it up, in the port itself, and from taking it up to letting go. `$DEFF` reads `F`.

To run it:

1. Flash the TR+ with a build of this branch.
2. Put `floathandoff` on SD or USB. The disk image built alongside it also has `fmultbench`
   and `sineplot`, for native timings on the same machine.
3. In the TR menu, set the Special IO to **Debug**, then launch `floathandoff`.

It runs for about ten seconds with the screen blank, then shows:

- the ping times, polled and held, in C64 cycles (min, avg, max);
- per routine, the average C64 cycles natively and answered by the TR+, and how many of the
  256 answers differed from the ROM's;
- per routine, the TR's average latency, compute and whole-job times in nS.

The held cycles include the DMA's own reads and writes. The real hook's served code and RTS,
about 15–25 cycles, are not included.

`floathandoff-run.test.mjs` runs the program on the ROMs against a model of the Debug
handler, so its logic is checked off the board. On a TR, the held commands report that
there is no DMA.

## Running the ROM on the Teensy

The hooks above leave two costs they cannot remove. Every hooked call pays a hand-off, 30% of
what is left of the sine plot. And the interpreter itself, about a third of the rest, does not
break into routines a hook could take. The other design runs the BASIC and KERNAL ROMs on the
Teensy, against a copy of the C64's RAM, with the 6510 held as the REU holds it:

- **RAM is a copy.** The 6502 reads and writes it at the Teensy's speed. A dirty bitmap marks
  what it writes, and about once a frame the bytes the VIC displays go to the C64. Those are
  the screen, the character set or bitmap, and the enabled sprites, by the `$DD00`, `$D018`,
  `$D011` and `$D015` values last written. Everything else stays dirty until the C64 gets its
  CPU back (`BasicEmuFlushAll`).
- **I/O goes to the bus** (`$D000-$DFFF` while the port maps it in), one access for each the
  6502 makes. Reads stall the 6502; writes need not. A read-modify-write on I/O writes the old
  value first, as the NMOS part does (`INC $D019` relies on it).
- **The 6510's port** (`$00/$01`) is inside the CPU, where DMA cannot reach, so the banking is
  modeled.
- **The hooks are direct calls.** `BasicEmuInit` puts a JAM in the copy of the ROM at each
  entry in the table above. The 6502 stops there and calls the C port with the real registers.
  The operand entries need no `STA $DE00` and FIN no text window. If the port declines, the
  instruction it covered runs. There is no hand-off.
- **Interrupts** come from the C64's chips through `Cpu6502IrqLine`, since with the 6510 held
  nothing else takes them. The KERNAL's IRQ runs on the 6502 here, keyboard scan included.

`Cpu6502.h` runs the 151 documented opcodes and counts the cycles each would take on the real
part. It stops at a JAM or any other undocumented opcode, and makes none of the real part's
dummy reads. `cpu6502.test.js` checks every documented opcode against `mos6502.mjs`: registers,
flags, cycles and every write, from random states steered onto page crossings, wraps and
interrupts. `basic-emu.test.js` checks the banking, the bus, the hooks and the flush on
made-up ROMs.

### The model

`emu-model.mjs` boots the real ROMs in `emu_model.c`, which stands in for the C64 on the other
side of the bus. It models CIA1's timer A and its IRQ, the VIC's raster and its latch (the
KERNAL's PAL/NTSC check reads both), and every other register as plain storage. At BASIC's first
prompt the model puts the listing in memory and types `RUN`.

Time is the C64's clock. The 6502 runs `--speed` native cycles per C64 cycle. A bus read
stalls it `--read` cycles, a bus write or flushed byte costs `--write`, and an answered hook
costs `--hook`. `--native` is the C64's own CPU: speed 1, the bus free, and the VIC's badline
cycles taken.

The native run checks the model: the plot takes 2284 jiffies (38.1 s), against VICE's 38.5 s.
Every run below draws the same bitmap, and no hook handed back to the ROM. "RUN to done"
includes clearing the bitmap and screen, which the program's own TI leaves out. A write costs
1 cycle and a hook 2.

| Run | RUN to done | Speedup | Plot, in TI jiffies | Bus reads | Bus writes |
|---|---|---|---|---|---|
| native | 72.6 s | 1.0× | 2284 | 17302 | 8655 |
| 10×, no hooks | 6.83 s | 10.6× | 191 | 1450 | 18514 |
| 10×, hooks | 2.59 s | 28.1× | 44 | 554 | 18000 |
| 20×, no hooks | 3.42 s | 21.2× | 95 | 726 | 17801 |
| 20×, hooks | 1.37 s | 53.2× | 23 | 294 | 17490 |
| 40×, no hooks | 1.72 s | 42.2× | 32 | 246 | 18375 |
| 40×, hooks | 0.76 s | 96.0× | 10 | 122 | 18494 |

The read cost is 3 cycles in that table. At 75, roughly the cost of waking `loop()` for each
read, no run changes by more than 3%. The plot makes about 13 reads a jiffy, all of them the
KERNAL's IRQ acknowledging CIA1 and scanning the keyboard. Nearly all the writes are bitmap and
screen bytes flushed to the VIC. The hooks remove 65% of the 6502's cycles (7.3 million
instructions from RUN, against 20.4 million). So what decides the result is the emulator's own
speed, which the model only assumes.

### What it does not do yet

- **Measure its speed on the Teensy.** That depends on `isrPHI2`, which still runs every cycle
  while the 6510 is held, and on where the code and the 92 KB of state live. The firmware
  would want the core in flash (`FLASHMEM`) and the state in RAM2 (`DMAMEM`). On the host it
  runs about 90 million instructions a second.
- **Start from a running C64.** The model boots from reset. On the board, entry would copy 64K
  by DMA, read the VIC's and CIA2's registers into `IoLast`, and take the 6510's registers
  from the C64.
- **Give the C64 its CPU back.** The serial bus, and anything that counts cycles, needs the
  real 6510. That means flushing every dirty byte, serving the registers back from IO1, and
  catching the return.
- **Keep time.** Raster effects, SID playback and anything else timed run at the 6502's
  speed, not the C64's.
- **Cartridges, undocumented opcodes, the NMI.** None is modeled.

## Running it

```bash
npm run build:c64 -- --project FMultBench
```

```bash
node tools/basic-accel/vice-float.mjs --program bench
```

```bash
node tools/basic-accel/vice-float.mjs --program sine
```

```bash
node tools/basic-accel/vice-float.mjs --program sine --model tr-plus --profile
```

```bash
node tools/basic-accel/emu-model.mjs --speed 10,20,40 --read 3,75
```

`emu-model.mjs` needs only a C compiler and the BASIC, KERNAL and character ROMs, found as VICE
ships them or through `C64_BASIC_ROM`, `C64_KERNAL_ROM` and `C64_CHARGEN_ROM`. The VICE scripts
need `x64sc` with the C64 ROMs and symbol file that VICE ships (looked for in
`/usr/share/vice/C64`), a C compiler, and `petcat` for the sine plot. On a machine without a
display it also needs `xvfb-run`. `--routines` limits which routines are hooked, by the names in `ROUTINES` in
`float-cases.mjs` (`faddt`, `fadd`, `fin`, `qint` and so on). Some Linux
packages of VICE leave the ROMs out; `C64_BASIC_ROM` points the test at a copy of
`basic-901226-01.bin`.
