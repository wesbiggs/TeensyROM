# BASIC float acceleration

An experiment in answering BASIC V2's slow float routines from the cartridge, transparently to
the C64 program. So far it covers the four arithmetic routines (add, subtract, multiply and
divide) and FIN, which parses a number from text. Nothing here is in the firmware yet. This is
the C ports, the evidence that they are bit-exact, and a model of what they would gain.

| File | What it is |
|---|---|
| `Source/Teensy/BasicAccel/CbmFloat.h` | FADDT, FSUBT, FMULTT, FDIVT and FIN in C, bit-exact with the ROM |
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
| FIN | `$BCF3` | the number at TXTPTR | a number in program text (`$AE8F`, `$AEAD`); INPUT and READ (`$AC89`); VAL (`$B7DA`) |

At each arithmetic entry, every input is in zero page (`$61-$70`), and every caller leaves N/Z
set from `$61`. Every caller of these entries in the BASIC and KERNAL ROMs was checked for that.
A cartridge can read memory with DMA. It cannot see A, X, Y or the flags. So each port reports
which registers and flags it determined (`Set`). The test checks that the ROM leaves every
other one as the caller had it.

The ports hand back to the ROM (`CbmFloat_Native`) only when the ROM would raise an error:
`?OVERFLOW ERROR` or `?DIVISION BY ZERO ERROR`.

The arithmetic writes `$26-$2A`, `$56` and `$61-$70`. `$2A` is FDIVT's: its last quotient
store, `STA $29,X` with X = 1, lands one byte past the product.

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
| `tr-plus` | ~116–126 | JMP into IO1 (3), thread hand-off (75), /DMA read of `$61-$70` (16; for FIN, `$68`, CHRGET and the text it used) and write-back of the changed bytes, bus turn (4), served register loads and RTS |
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

SinePlot.bas, plotting 320 points, with all five routines hooked. The plot is the same bitmap
in every run.

| Model | Plot time | Speedup |
|---|---|---|
| native | 38.5 s | 1.0× |
| `tr-plus` | 13.8 s | 2.79× |
| `tr` | 17.0 s | 2.26× |
| `tr-plus-isr` | 12.0 s | 3.21× |

The same plot on `tr-plus`, adding one routine at a time:

| Hooked | Plot time |
|---|---|
| none | 38.5 s |
| FMULT | 26.8 s |
| + FDIV | 21.7 s |
| + FADD | 20.4 s |
| + FSUB | 20.4 s |
| + FIN | 13.8 s |

FADD is called most often but gains least. It is short natively, so a 116-cycle hand-off
leaves little of the saving. With a cheaper hand-off it gains much more. FIN gains most: every
constant in the program (`8192`, `320`, `50.93`) is parsed digit by digit, a MUL10 and an add
per digit, each time its line runs. Hooked, FIN's ~12,300 calls take the ~18,000 adds and the
DIV10 divides with them.

## What is left

`--profile` samples the C64 at every KERNAL IRQ (50 a second on PAL). It records the routine
that was running and the routines that called it, from the JSR return addresses on the stack.
The sine plot on `tr-plus` with all five routines hooked, as a share of the remaining 13.8 s:

| Self time | Routine | What it is |
|---|---|---|
| 22% | (cartridge) | the modeled hand-offs themselves |
| 14% | FRMEVL | the expression evaluator |
| 9% | CONUPK | unpack a float from memory into ARG |
| 8% | MULSHF | QINT (`INT`, `AND`, POKE addresses) shifting by itself |
| 8% | CHRGET | the interpreter reading program text, in RAM |
| 6% | FADD region | FADDT's body and NORMLZ, entered below the hook by other routines |
| 3% | ORDVAR | variable lookup |

The candidates, roughly in order:

1. **A cheaper hand-off.** It is now the largest single item, and every further hook pays it.
2. **Folding CONUPK into the hooks.** Hooking FMULT at `$BA28` instead of `$BA2B` (and FADD,
   FSUB and FDIV at theirs) saves CONUPK's time, but needs the A/Y pointer. Served code can
   store those registers to IO1 in 8 cycles.
3. **QINT** (`$BC9B`) and **AYINT** (`$B1BF`). They work in zero page only, like the
   arithmetic.
4. **MOVAF, MOVFM, MOV2F.** They are short. They only pay off with a cheap hand-off, or merged
   into a neighboring hook.

The interpreter itself (FRMEVL, CHRGET, EVAL, PTRGET/ORDVAR, about a third of what is left)
does not break into routines a hook could take.

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
