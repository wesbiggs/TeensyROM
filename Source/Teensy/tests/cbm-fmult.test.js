// SPDX-License-Identifier: MIT
// CbmFMult.h against the C64 BASIC ROM's FMULTT ($BA2B): through the committed vectors always, and
// against the ROM itself, run in mos6502.mjs, where a copy is found (see findBasicRom).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';
import { FLAG } from '../../../tools/lib/mos6502.mjs';
import { findBasicRom, makeCases, runRom, caseLine, resultLine } from '../../../tools/basic-accel/fmult-cases.mjs';

const FIRMWARE_ROOT = path.resolve(import.meta.dirname, '..');
const HARNESS_SOURCE = path.join(FIRMWARE_ROOT, 'tests/cbm_fmult_harness.c');
const VECTORS = path.join(FIRMWARE_ROOT, 'tests/cbm-fmult-vectors.txt');
const SEED = 0x6502;

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('cc') && !inCi && 'cc is not on PATH';
const rom = findBasicRom();
const skipRom = skip || (!rom && 'no C64 BASIC ROM 901226-01 (install VICE with its ROMs, or set C64_BASIC_ROM)');

const SET = { A: 0x100, X: 0x200, Y: 0x400 };
const FLAGS = FLAG.N | FLAG.V | FLAG.Z | FLAG.C;

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cbm-fmult-'));
  harnessPath = path.join(dir, 'cbm_fmult_harness');
  const built = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function port(cases) {
  const ran = spawnSync(harness(), [], { input: cases.map(caseLine).join('\n') + '\n', encoding: 'utf8', maxBuffer: 1 << 30 });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.trim().split('\n').map((line) => {
    const [result, set, a, x, y, p, zp] = line.split(' ');
    const [A, X, Y, P] = [a, x, y, p].map((h) => parseInt(h, 16));
    return { result, set: parseInt(set, 16), a: A, x: X, y: Y, p: P, zp: Buffer.from(zp, 'hex') };
  });
}

test('the port gives the ROM\'s results on the committed vectors', { skip }, () => {
  const [header, ...listed] = fs.readFileSync(VECTORS, 'utf8').split('\n').filter((l) => l && !l.startsWith('#'));
  const [, count, hash] = header.split(' ');
  const results = port(makeCases(Number(count), SEED)).map(resultLine);
  listed.forEach((want, i) => assert.equal(results[i], want, `case ${i}`));
  assert.equal(createHash('sha256').update(results.join('\n')).digest('hex'), hash, `the ${count} hashed cases`);
});

test('the port reads only $61-$70 and writes only $26-$29, $61-$66 and $70', { skip }, () => {
  const cases = makeCases(2000, 0x1541);
  const scrambled = cases.map((c) => {
    const zp = c.zp.map((v, at) => (at >= 0x61 && at <= 0x70 ? v : v ^ 0x5a));
    return { ...c, zp };
  });
  const plain = port(cases);
  const other = port(scrambled);
  const written = new Set([0x26, 0x27, 0x28, 0x29, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x70]);
  plain.forEach((r, i) => {
    const o = other[i];
    assert.deepEqual([o.result, o.a, o.x, o.y, o.p], [r.result, r.a, r.x, r.y, r.p], `case ${i}`);
    for (let at = 0; at < 256; at++) {
      const left = r.zp[at] === cases[i].zp[at] && o.zp[at] === scrambled[i].zp[at];
      if (!written.has(at) || r.result === 'native') assert.ok(left, `case ${i} wrote $${at.toString(16)}`);
      else if (!left) assert.equal(o.zp[at], r.zp[at], `case ${i} $${at.toString(16)} depends on a byte outside $61-$70`);
    }
  });
});

test('the port matches the ROM on every zero-page byte, register and flag', { skip: skipRom }, () => {
  const cases = makeCases(20_000, 0x0801);
  port(cases).forEach((r, i) => {
    const want = runRom(rom, cases[i]);
    assert.equal(r.result, want.result, `case ${i}: ${caseLine(cases[i])}`);
    if (r.result === 'native') return;
    assert.deepEqual([r.a, r.x, r.y, r.p], [want.a, want.x, want.y, want.p], `case ${i} registers`);
    assert.deepEqual([...r.zp], [...want.zp], `case ${i} zero page`);
  });
});

// What a cartridge returns to the C64 is only what the call determined: X, Y, C and V are not
// visible to it on entry. So the ROM must leave each one it did not determine as it found it.
test('what the port leaves undetermined, the ROM leaves as the caller had it', { skip: skipRom }, () => {
  const cases = makeCases(5_000, 0x0c64);
  port(cases).forEach((r, i) => {
    if (r.result === 'native') return;
    const c = cases[i];
    const flipped = { ...c, x: c.x ^ 0xff, y: c.y ^ 0xff, p: c.p ^ (FLAG.C | FLAG.V) };
    const a = runRom(rom, c);
    const b = runRom(rom, flipped);
    for (const [reg, bit] of [['x', SET.X], ['y', SET.Y]]) {
      if (r.set & bit) assert.equal(b[reg], a[reg], `case ${i} ${reg} depends on the caller`);
      else assert.equal(b[reg], flipped[reg], `case ${i} ${reg} changed but is not in Set`);
    }
    for (const flag of [FLAG.C, FLAG.V]) {
      if (r.set & flag) assert.equal(b.p & flag, a.p & flag, `case ${i} flag ${flag} depends on the caller`);
      else assert.equal(b.p & flag, flipped.p & flag, `case ${i} flag ${flag} changed but is not in Set`);
    }
    assert.equal(r.set & (SET.A | FLAG.N | FLAG.Z), SET.A | FLAG.N | FLAG.Z, `case ${i}: every result sets A, N and Z`);
    assert.equal(a.p & ~FLAGS, c.p & ~FLAGS, `case ${i} I, D or B changed`);
  });
});
