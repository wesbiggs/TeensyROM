# BASIC float acceleration: FMULT

An experiment in answering BASIC V2's slow float routines from the cartridge, transparently to
the C64 program. The first routine is the multiply, FMULTT at `$BA2B`. Nothing here is in the
firmware yet: this is the C port, the evidence that it is bit-exact, and a model of what it
would gain.

| File | What it is |
|---|---|
| `Source/Teensy/BasicAccel/CbmFMult.h` | FMULTT in C, bit-exact with the ROM |
| `Source/Teensy/tests/cbm-fmult.test.js` | checks it against the ROM (`npm test`) |
| `Source/Teensy/tests/cbm-fmult-vectors.txt` | the ROM's results, so CI checks it without the ROM |
| `tools/basic-accel/fmult-cases.mjs` | the test cases, and the ROM routine run in `tools/lib/mos6502.mjs` |
| `tools/basic-accel/gen-fmult-vectors.mjs` | rewrites the vectors file from the ROM |
| `Source/C64/FMultBench/` | C64 benchmark: 4,500 FMULTs, timed with CIA2 |
| `tools/basic-accel/SinePlot.bas` | a type-in style hi-res sine plot in BASIC |
| `tools/basic-accel/vice-fmult.mjs` | runs either one in VICE, natively or with a modeled cartridge |

## Why `$BA2B`

Both of the ROM's routes into the multiply meet there. FMULT (`$BA28`, A/Y pointing at a
float in memory) arrives after CONUPK has unpacked that float into ARG. The expression
evaluator's `*` arrives by RTS from `$AE80`. Either way, the inputs are all in zero page
(`$61-$70`), A holds `$61`, and N and Z come from it. A cartridge can read the zero page with
DMA. It cannot see X, Y, C or V. So `CbmFMultT` reports which registers and flags it
determined (`Set`). The test checks that the ROM leaves every other one as the caller had it.

`CbmFMultT` hands two cases back to the ROM (`CbmFMult_Native`). One is FAC = 0, where the ROM
returns after a branch. The other is an overflow, where the ROM raises `?OVERFLOW ERROR` itself.

## Bit-exact includes the ROM's quirk

The multiply runs over the five bytes of FAC's mantissa, low byte first, and sends a zero byte
to MULSHF (`$B983`). If MULSHF is entered with carry clear, it also shifts the partial product
one bit right, as a signed value, and drops the bit that falls out. That happens for a second
zero byte in a row, or for a zero rounding byte, depending on the exponent sum. The C port
reproduces it on purpose. Without it, 17% of the random test cases differ from the ROM in the
low bits.

The test runs 20,000 cases against the ROM and compares every zero-page byte, register and
flag. The vectors file holds the ROM's results for 50,000 more. In VICE, the benchmark's
checksum and the sine plot's bitmap are identical with and without the hook.

## The VICE model

VICE has no TeensyROM, so `vice-fmult.mjs` uses VICE's binary monitor to stand in for the
cartridge:

1. An exec checkpoint stops the C64 at `$BA2B`.
2. The script reads the zero page and the registers, runs the C port, and writes the result back.
3. It points the PC at a run of NOPs ending in RTS. The run takes as many cycles as the modeled
   cartridge would hold the C64 for.

The C64's own clock then measures the modeled run. While the monitor holds the C64, the C64
does not run, so the script's own time costs it nothing.

The cycle costs are a model (`COSTS` in the script), not a measurement:

| Model | Per call | Made of |
|---|---|---|
| `tr-plus` | ~126 | JMP into IO1 (3), thread hand-off (75), /DMA read of `$61-$70` (16) and write-back (11), bus turn (4), served `LDX/LDY/CLC/CLV/LDA #` and RTS (17) |
| `tr` | ~262 | no DMA: the CPU moves the bytes from served code, `LDA zp / STA $DExx` (7 per byte) and `LDA # / STA zp` (5 per byte) |
| `tr-plus-isr` | ~52 | `tr-plus` with no thread hand-off: a lower bound, not a design |

The hand-off figure is the 70–80 cycles that [Constraints.md](../../docs/Architecture/Constraints.md)
gives for a WriteC64Spans slice. Only a board can say what this path costs.

## Results (VICE 3.7.1, x64sc, PAL)

FMultBench, 4,500 calls. Each call includes the JSR and CONUPK, which still run natively.

| Model | FMULT cycles/call | Speedup on FMULT | Speedup on the whole pass |
|---|---|---|---|
| native | 2026 | 1.0× | 1.0× (12.26 s) |
| `tr-plus` | 222 | 9.1× | 3.0× |
| `tr` | 368 | 5.5× | 2.6× |
| `tr-plus-isr` | 143 | 14.2× | 3.4× |

SinePlot.bas, plotting 320 points: 7,357 multiplies are answered and 363 (FAC = 0) are handed back.

| Model | Plot time | Speedup |
|---|---|---|
| native | 38.5 s | 1.0× |
| `tr-plus` | 26.8 s | 1.44× |
| `tr` | 27.8 s | 1.39× |
| `tr-plus-isr` | 26.2 s | 1.47× |

The plot gains much less than the multiplies themselves, because the multiplies are only about
a third of its time. The rest goes to FDIV (`X/50.93`, `Y/8`, `X/8`), FADD, INT, the
`2^` power, and the interpreter itself. Accelerating FDIV and FADD the same way is the next
step to measure. After that, the cost of the hand-off dominates, which is the same as asking
the ROM to do less work per call.

## Running it

```bash
npm run build:c64 -- --project FMultBench
```

```bash
node tools/basic-accel/vice-fmult.mjs --program bench
```

```bash
node tools/basic-accel/vice-fmult.mjs --program sine
```

This needs `x64sc` with the C64 ROMs that VICE ships, a C compiler, and `petcat` for the sine
plot. On a machine without a display it also needs `xvfb-run`. Some Linux packages of VICE
leave the ROMs out; `C64_BASIC_ROM` points the test at a copy of `basic-901226-01.bin`.
