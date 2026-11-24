// SPDX-License-Identifier: MIT
// The listing tokenizer always; the model on the C64's ROMs where they are installed: the sine
// plot draws the same bitmap natively and on BasicEmu.h, hooked or not, and no hook hands back.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';

import { commandExists } from '../lib/toolchain.mjs';
import { crunch, tokenize } from './basic-tokenize.mjs';
import { findRoms, prepare, SINE_PLOT, ALL_HOOKS } from './emu-model.mjs';

const bytes = (text) => [...text].map((c) => c.charCodeAt(0));

test('crunch tokenizes keywords outside strings, REM and DATA, as the ROM does', () => {
  assert.deepEqual(crunch('poke 53265,peek(53265)or32'),
    [0x97, 0x20, ...bytes('53265,'), 0xc2, ...bytes('(53265)'), 0xb0, ...bytes('32')]);
  assert.deepEqual(crunch('for i=8192 to 16191'), [0x81, 0x20, 0x49, 0xb2, ...bytes('8192 '), 0xa4, 0x20, ...bytes('16191')]);
  assert.deepEqual(crunch('fort=1'), [0x81, 0x54, 0xb2, 0x31], 'FOR wins over the variable T');
  assert.deepEqual(crunch('?"to":rem to'), [0x99, 0x22, 0x54, 0x4f, 0x22, 0x3a, 0x8f, 0x20, 0x54, 0x4f]);
  assert.deepEqual(crunch('data to,"a:b":to'), [0x83, 0x20, 0x54, 0x4f, 0x2c, 0x22, 0x41, 0x3a, 0x42, 0x22, 0x3a, 0xa4]);
  assert.deepEqual(crunch('A'), [0xc1], 'uppercase is shifted PETSCII');
});

test('tokenize links the lines from $0801 and ends with a zero link', () => {
  const prg = tokenize('10 end\n20 goto 10\n');
  assert.deepEqual([...prg], [0x01, 0x08, 0x07, 0x08, 10, 0, 0x80, 0, 0x10, 0x08, 20, 0, 0x89, 0x20, 0x31, 0x30, 0, 0, 0]);
  assert.throws(() => tokenize('64000 end'), /over 63999/);
});

const roms = findRoms();
const skip = (!commandExists('cc') && 'cc is not on PATH') || (!roms && 'no C64 ROMs (install VICE with its ROMs, or set C64_BASIC_ROM, C64_KERNAL_ROM and C64_CHARGEN_ROM)');

test('the sine plot is the same bitmap natively and on BasicEmu, hooked or not', { skip }, () => {
  const run = prepare(roms, fs.readFileSync(SINE_PLOT, 'utf8'));
  const native = run({ native: true });
  const plain = run({ speed: 20, hooks: 0 });
  const hooked = run({ speed: 20, hooks: ALL_HOOKS });
  assert.ok(native.jiffies > 1500, `the native plot took ${native.jiffies} jiffies`);
  assert.equal(plain.bitmap, native.bitmap);
  assert.equal(hooked.bitmap, native.bitmap);
  assert.equal(hooked.screen, native.screen);
  assert.ok(hooked.hookCalls.every((n) => n > 0), `a routine was never hooked: ${hooked.hookCalls}`);
  assert.deepEqual(hooked.hookNative, hooked.hookNative.map(() => 0));
  assert.ok(hooked.instructions < plain.instructions / 2);
});

test('the jiffy clock keeps the C64\'s time at every speed', { skip }, () => {
  const run = prepare(roms, fs.readFileSync(SINE_PLOT, 'utf8'));
  // the KERNAL's PAL timer A latch, $4025: one IRQ every 16422 cycles
  for (const options of [{ native: true }, { speed: 10 }, { speed: 40 }]) {
    const r = run(options);
    // a jiffy either side for where RUN and the end fall in the period
    assert.ok(Math.abs(r.runJiffies - r.runCycles / 16422) <= 1.5, `${JSON.stringify(options)}: ${r.runJiffies} jiffies in ${r.runCycles} cycles`);
  }
});
