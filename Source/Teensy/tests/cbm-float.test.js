// SPDX-License-Identifier: MIT
// CbmFloat.h against the C64 BASIC ROM's float routines: through the committed vectors always,
// and against the ROM itself, run in mos6502.mjs, where a copy is found (see findBasicRom).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';
import { FLAG } from '../../../tools/lib/mos6502.mjs';
import { findBasicRom, makeCases, runRom, caseLine, resultLine, ROUTINES } from '../../../tools/basic-accel/float-cases.mjs';

const FIRMWARE_ROOT = path.resolve(import.meta.dirname, '..');
const HARNESS_SOURCE = path.join(FIRMWARE_ROOT, 'tests/cbm_float_harness.c');
const VECTORS = path.join(FIRMWARE_ROOT, 'tests/cbm-float-vectors.txt');
const SEED = 0x6502;

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('cc') && !inCi && 'cc is not on PATH';
const rom = findBasicRom();
const skipRom = skip || (!rom && 'no C64 BASIC ROM 901226-01 (install VICE with its ROMs, or set C64_BASIC_ROM)');

const SET = { A: 0x100, X: 0x200, Y: 0x400 };
const FLAGS = FLAG.N | FLAG.V | FLAG.Z | FLAG.C;

const WRITES = new Set([0x26, 0x27, 0x28, 0x29, 0x2a, 0x56, ...Array.from({ length: 0x10 }, (_, i) => 0x61 + i)]);

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cbm-float-'));
  harnessPath = path.join(dir, 'cbm_float_harness');
  const built = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function port(routine, cases) {
  const input = cases.map((c) => caseLine(routine, c)).join('\n') + '\n';
  const ran = spawnSync(harness(), [], { input, encoding: 'utf8', maxBuffer: 1 << 30 });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.trim().split('\n').map((line) => {
    const [result, set, a, x, y, p, zp] = line.split(' ');
    const [A, X, Y, P] = [a, x, y, p].map((h) => parseInt(h, 16));
    return { result, set: parseInt(set, 16), a: A, x: X, y: Y, p: P, zp: Buffer.from(zp, 'hex') };
  });
}

function vectors() {
  const byRoutine = {};
  for (const line of fs.readFileSync(VECTORS, 'utf8').split('\n')) {
    if (!line || line.startsWith('#')) continue;
    const [first, ...rest] = line.split(' ');
    if (first === 'sha256') {
      const [routine, count, hash] = rest;
      byRoutine[routine] = { count: Number(count), hash, listed: [] };
    }
    else byRoutine[first].listed.push(rest.join(' '));
  }
  return byRoutine;
}

for (const routine of Object.keys(ROUTINES)) {
  test(`${routine}: the port gives the ROM's results on the committed vectors`, { skip }, () => {
    const want = vectors()[routine];
    assert.ok(want, `no vectors for ${routine}`);
    const results = port(routine, makeCases(want.count, SEED)).map(resultLine);
    want.listed.forEach((line, i) => assert.equal(results[i], line, `case ${i}`));
    assert.equal(createHash('sha256').update(results.join('\n')).digest('hex'), want.hash, `the ${want.count} hashed cases`);
  });

  test(`${routine}: the port reads only $61-$70 and writes only $26-$2A, $56 and $61-$70`, { skip }, () => {
    const cases = makeCases(2000, 0x1541);
    const keep = (at) => at >= 0x61 && at <= 0x70;
    const scrambled = cases.map((c) => ({ ...c, zp: c.zp.map((v, at) => (keep(at) ? v : v ^ 0x5a)) }));
    const plain = port(routine, cases);
    const other = port(routine, scrambled);
    plain.forEach((r, i) => {
      const o = other[i];
      assert.deepEqual([o.result, o.set, o.a, o.x, o.y, o.p], [r.result, r.set, r.a, r.x, r.y, r.p], `case ${i}`);
      if (r.result === 'native') return;  // nothing is written back
      for (let at = 0; at < 256; at++) {
        const left = r.zp[at] === cases[i].zp[at] && o.zp[at] === scrambled[i].zp[at];
        if (!WRITES.has(at)) assert.ok(left, `case ${i} wrote $${at.toString(16)}`);
        else if (!left) assert.equal(o.zp[at], r.zp[at], `case ${i} $${at.toString(16)} depends on a byte it should not read`);
      }
    });
  });

  test(`${routine}: the port matches the ROM on every zero-page byte, register and flag`, { skip: skipRom }, () => {
    const cases = makeCases(10_000, 0x0801);
    port(routine, cases).forEach((r, i) => {
      const want = runRom(rom, routine, cases[i]);
      assert.equal(r.result, want.result, `case ${i}: ${caseLine(routine, cases[i])}`);
      if (r.result === 'native') return;
      assert.deepEqual([r.a, r.x, r.y, r.p], [want.a, want.x, want.y, want.p], `case ${i} registers`);
      assert.deepEqual([...r.zp], [...want.zp], `case ${i} zero page`);
    });
  });

  // A cartridge returns to the C64 only what the call determined, since it cannot see the
  // registers or flags on entry. So the ROM must leave each one the port did not determine as
  // it found it. N/Z stay set from $61, as every caller leaves them; A need not be $61.
  test(`${routine}: what the port leaves undetermined, the ROM leaves as the caller had it`, { skip: skipRom }, () => {
    const cases = makeCases(3_000, 0x0c64);
    port(routine, cases).forEach((r, i) => {
      if (r.result === 'native') return;
      const c = cases[i];
      const flipped = { ...c, a: c.a ^ 0xff, x: c.x ^ 0xff, y: c.y ^ 0xff, p: c.p ^ (FLAG.C | FLAG.V) };
      const a = runRom(rom, routine, c);
      const b = runRom(rom, routine, flipped);
      assert.deepEqual([...b.zp], [...a.zp], `case ${i}: the zero page depends on the caller's registers`);
      for (const [reg, bit] of [['a', SET.A], ['x', SET.X], ['y', SET.Y]]) {
        if (r.set & bit) assert.equal(b[reg], a[reg], `case ${i} ${reg} depends on the caller`);
        else assert.equal(b[reg], flipped[reg], `case ${i} ${reg} changed but is not in Set`);
      }
      for (const flag of [FLAG.N, FLAG.V, FLAG.Z, FLAG.C]) {
        if (r.set & flag) assert.equal(b.p & flag, a.p & flag, `case ${i} flag ${flag} depends on the caller`);
        else assert.equal(b.p & flag, flipped.p & flag, `case ${i} flag ${flag} changed but is not in Set`);
      }
      assert.equal(a.p & ~FLAGS, c.p & ~FLAGS, `case ${i} I, D or B changed`);
    });
  });
}
