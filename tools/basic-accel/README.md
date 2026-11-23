# BASIC float acceleration

An experiment in answering BASIC V2's slow float routines from the cartridge, transparently to
the C64 program. So far it covers the four arithmetic routines: add, subtract, multiply and
divide. Nothing here is in the firmware yet. This is the C ports, the evidence that they are
bit-exact, and a model of what they would gain.

| File | What it is |
|---|---|
| `Source/Teensy/BasicAccel/CbmFloat.h` | FADDT, FSUBT, FMULTT and FDIVT in C, bit-exact with the ROM |
| `Source/Teensy/tests/cbm-float.test.js` | checks them against the ROM (`npm test`) |
| `Source/Teensy/tests/cbm-float-vectors.txt` | the ROM's results, so CI checks them without the ROM |
| `tools/basic-accel/float-cases.mjs` | the test cases, and the ROM routines run in `tools/lib/mos6502.mjs` |
| `tools/basic-accel/gen-float-vectors.mjs` | rewrites the vectors file from the ROM |
| `Source/C64/FMultBench/` | C64 benchmark: 4,500 FMULTs, timed with CIA2 |
| `tools/basic-accel/SinePlot.bas` | a type-in style hi-res sine plot in BASIC |
| `tools/basic-accel/vice-float.mjs` | runs either one in VICE, natively or with a modeled cartridge, optionally profiling |

## Where the hooks go

| Routine | Hooked at | Computes | Callers that meet there |
|---|---|---|---|
| FADDT | `$B86A` | ARG + FAC | `+`; FADD (`$B867`) after CONUPK; FIN's digit loop (`$BD8E`) |
| FSUBT | `$B853` | ARG − FAC | `-`; FSUB (`$B850`) after CONUPK; SIN and others |
| FMULTT | `$BA2B` | ARG × FAC | `*`; FMULT (`$BA28`) after CONUPK |
| FDIVT | `$BB12` | ARG ÷ FAC | `/`; FDIV (`$BB0F`) after CONUPK; DIV10 |

At each entry, every input is in zero page (`$61-$70`), and every caller leaves N/Z set from
`$61`. Every caller of these four entries in the BASIC and KERNAL ROMs was checked for that.
A cartridge can read the zero page with DMA. It cannot see A, X, Y or the flags. So each port
reports which registers and flags it determined (`Set`). The test checks that the ROM leaves
every other one as the caller had it.

The ports hand back to the ROM (`CbmFloat_Native`) only when the ROM would raise an error:
`?OVERFLOW ERROR` or `?DIVISION BY ZERO ERROR`.

The routines write `$26-$2A`, `$56` and `$61-$70`. `$2A` is FDIVT's: its last quotient store,
`STA $29,X` with X = 1, lands one byte past the product.

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
separate run of 50,000 cases per routine also matched. In VICE, the benchmark's checksum and
the sine plot's bitmap are identical with and without the hooks.

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
| `tr-plus` | ~116–126 | JMP into IO1 (3), thread hand-off (75), /DMA read of `$61-$70` (16) and write-back of the changed bytes, bus turn (4), served register loads and RTS |
| `tr` | ~225–260 | no DMA: the CPU moves the bytes from served code, `LDA zp / STA $DExx` (7 per byte) and `LDA # / STA zp` (5 per byte) |
| `tr-plus-isr` | ~42–52 | `tr-plus` with no hand-off to thread mode: the port runs inside the PHI2 interrupt while the C64 is held |

The hand-off figure is the 70–80 cycles that [Constraints.md](../../docs/Architecture/Constraints.md)
gives for a WriteC64Spans slice. `tr-plus-isr` breaks the rule against new work on the PHI2
path, so it is a bound, not a design. Only a board can say what any of this costs.

## Results (VICE 3.7.1, x64sc, PAL)

FMultBench, 4,500 calls. Each call includes the JSR and CONUPK, which still run natively.

| Model | FMULT cycles/call | Speedup on FMULT | Speedup on the whole pass |
|---|---|---|---|
| native | 2026 | 1.0× | 1.0× (12.26 s) |
| `tr-plus` | 222 | 9.1× | 3.1× |
| `tr` | 365 | 5.5× | 2.6× |
| `tr-plus-isr` | 142 | 14.3× | 3.4× |

SinePlot.bas, plotting 320 points, with all four routines hooked: 35,664 adds, 1,881
subtracts, 7,720 multiplies and 2,201 divides. The plot is the same bitmap in every run.

| Model | Plot time | Speedup |
|---|---|---|
| native | 38.5 s | 1.0× |
| `tr-plus` | 20.4 s | 1.89× |
| `tr` | 23.7 s | 1.62× |
| `tr-plus-isr` | 18.3 s | 2.11× |

The same plot on `tr-plus`, adding one routine at a time:

| Hooked | Plot time |
|---|---|
| none | 38.5 s |
| FMULT | 26.8 s |
| + FDIV | 21.7 s |
| + FADD | 20.4 s |
| + FSUB | 20.4 s |

FADD is called most often but gains least. It is short natively, so a 116-cycle hand-off
leaves little of the saving. With a cheaper hand-off it gains much more.

## What is left

`--profile` samples the C64 at every KERNAL IRQ (50 a second on PAL). It records the routine
that was running and the routines that called it, from the JSR return addresses on the stack.
The sine plot on `tr-plus` with all four routines hooked, as a share of the remaining 20.4 s:

| Self time | Routine | What it is |
|---|---|---|
| 17% | (cartridge) | the modeled hand-offs themselves |
| 14% | FADD region | FADDT's body entered at `$B877` by MUL10, which FIN calls for every digit; NORMLZ from RND |
| 10% | FRMEVL | the expression evaluator |
| 9% | MOVAF | copy FAC to ARG with rounding |
| 7% | MULSHF | QINT (`INT`, `AND`, POKE addresses) shifting by itself |
| 7% | CHRGET | the interpreter reading program text, in RAM |
| 6% | CONUPK | unpack a float from memory into ARG |
| 6% | FIN | parsing numbers in the program text, every time a line runs |

FIN accounts for 24% of the time in total, counting what it calls. Every constant in the program
(`8192`, `320`, `50.93`) is parsed digit by digit each time its line runs.

The candidates, roughly in order:

1. **A cheaper hand-off.** It is now the largest single item, and every further hook pays it.
2. **FIN** (`$BCF3`), whole. It reads the program text at TXTPTR, outside zero page, so it
   needs a DMA read of RAM and has to leave TXTPTR where the ROM would.
3. **QINT** (`$BC9B`). It works in zero page only, like the four done so far.
4. **Folding CONUPK into the hook.** Hooking FMULT at `$BA28` instead of `$BA2B` saves CONUPK's
   time, but needs the A/Y pointer. Served code can store those registers to IO1 in 8 cycles.
5. **MOVAF, MOVFM, MOV2F.** They are short. They only pay off with a cheap hand-off, or merged
   into a neighboring hook.

The interpreter itself (FRMEVL, CHRGET, PTRGET/ORDVAR, about a quarter of what is left) does
not break into routines a hook could take.

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

This needs `x64sc` with the C64 ROMs and symbol file that VICE ships (looked for in
`/usr/share/vice/C64`), a C compiler, and `petcat` for the sine plot. On a machine without a
display it also needs `xvfb-run`. `--routines` limits which routines are hooked. Some Linux
packages of VICE leave the ROMs out; `C64_BASIC_ROM` points the test at a copy of
`basic-901226-01.bin`.
