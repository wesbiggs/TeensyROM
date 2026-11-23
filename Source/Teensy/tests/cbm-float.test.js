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
import { findBasicRom, casesFor, runRom, caseLine, resultLine, ROUTINES, TEXT_SLACK } from '../../../tools/basic-accel/float-cases.mjs';

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
const range = (from, to) => Array.from({ length: to - from + 1 }, (_, i) => from + i);

// What each routine may read and write in zero page, and what of the caller's state it relies on
// (so the test may not change it): the T entries take N/Z from $61; the operand entries take
// A/Y as the operand's address; FIN takes A and C from the text at TXTPTR.
const ARITHMETIC_WRITES = [0x26, 0x27, 0x28, 0x29, 0x2a, 0x56, ...range(0x61, 0x70)];
const T_ENTRY = {
  reads: () => range(0x61, 0x70),
  writes: ARITHMETIC_WRITES,
  flip: { a: 0xff, x: 0xff, y: 0xff, p: FLAG.C | FLAG.V },
};
const OPERAND_ENTRY = {
  reads: (c) => [...range(0x61, 0x70), ...range(c.memAt, c.memAt + 4).filter((at) => at < 0x100)],
  writes: [0x22, 0x23, ...ARITHMETIC_WRITES],
  flip: { a: 0, x: 0xff, y: 0, p: FLAG.C | FLAG.V | FLAG.N | FLAG.Z },
};
const INTEGER = {
  reads: () => range(0x61, 0x70),
  writes: [0x24, 0x25, 0x62, 0x63, 0x64, 0x65, 0x68, 0x70],
  flip: { a: 0xff, x: 0xff, y: 0xff, p: FLAG.C | FLAG.V | FLAG.N | FLAG.Z },
};
const SPEC = {
  faddt: T_ENTRY,
  fsubt: T_ENTRY,
  fmultt: T_ENTRY,
  fdivt: T_ENTRY,
  fadd: OPERAND_ENTRY,
  fsub: OPERAND_ENTRY,
  fmult: OPERAND_ENTRY,
  fdiv: OPERAND_ENTRY,
  fin: {
    reads: () => [0x68, ...range(0x73, 0x8a)],
    writes: [0x22, 0x23, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x56, ...range(0x5d, 0x70), 0x7a, 0x7b],
    flip: { a: 0, x: 0xff, y: 0xff, p: FLAG.V | FLAG.N | FLAG.Z },
  },
  qint: INTEGER,
  ayint: INTEGER,
};

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cbm-float-'));
  harnessPath = path.join(dir, 'cbm_float_harness');
  const built = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function port(routine, cases, line = caseLine) {
  const input = cases.map((c) => line(routine, c)).join('\n') + '\n';
  const ran = spawnSync(harness(), [], { input, encoding: 'utf8', maxBuffer: 1 << 30 });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.trim().split('\n').map((text) => {
    const [result, set, a, x, y, p, zp, used] = text.split(' ');
    const [A, X, Y, P] = [a, x, y, p].map((h) => parseInt(h, 16));
    return { result, set: parseInt(set, 16), a: A, x: X, y: Y, p: P, zp: Buffer.from(zp, 'hex'), used: Number(used) };
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
  const spec = SPEC[routine];

  test(`${routine}: the port gives the ROM's results on the committed vectors`, { skip }, () => {
    const want = vectors()[routine];
    assert.ok(want, `no vectors for ${routine}`);
    const results = port(routine, casesFor(routine, want.count, SEED)).map(resultLine);
    want.listed.forEach((line, i) => assert.equal(results[i], line, `case ${i}`));
    assert.equal(createHash('sha256').update(results.join('\n')).digest('hex'), want.hash, `the ${want.count} hashed cases`);
  });

  test(`${routine}: the port reads and writes only the zero page it should`, { skip }, () => {
    const cases = casesFor(routine, 2000, 0x1541);
    const writes = new Set(spec.writes);
    const scrambled = cases.map((c) => {
      const keep = new Set(spec.reads(c));
      return { ...c, zp: c.zp.map((v, at) => (keep.has(at) ? v : v ^ 0x5a)) };
    });
    const plain = port(routine, cases);
    const other = port(routine, scrambled);
    plain.forEach((r, i) => {
      const o = other[i];
      assert.deepEqual([o.result, o.set, o.a, o.x, o.y, o.p], [r.result, r.set, r.a, r.x, r.y, r.p], `case ${i}`);
      if (r.result === 'native') return;  // nothing is written back
      for (let at = 0; at < 256; at++) {
        const left = r.zp[at] === cases[i].zp[at] && o.zp[at] === scrambled[i].zp[at];
        if (!writes.has(at)) assert.ok(left, `case ${i} wrote $${at.toString(16)}`);
        else if (!left) assert.equal(o.zp[at], r.zp[at], `case ${i} $${at.toString(16)} depends on a byte it should not read`);
      }
    });
  });

  test(`${routine}: the port matches the ROM on every zero-page byte, register and flag`, { skip: skipRom }, () => {
    const cases = casesFor(routine, 10_000, 0x0801);
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
  // it found it. What the routine relies on from its caller stays as the caller leaves it.
  test(`${routine}: what the port leaves undetermined, the ROM leaves as the caller had it`, { skip: skipRom }, () => {
    const cases = casesFor(routine, 3_000, 0x0c64);
    port(routine, cases).forEach((r, i) => {
      if (r.result === 'native') return;
      const c = cases[i];
      const f = spec.flip;
      const flipped = { ...c, a: c.a ^ f.a, x: c.x ^ f.x, y: c.y ^ f.y, p: c.p ^ f.p };
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

// A cartridge reads the text a window at a time: a window too short must ask for more, never
// answer from what it has
test('fin: a text window shorter than FIN reads asks for more', { skip }, () => {
  const cases = casesFor('fin', 2000, 0x1571);
  const full = port('fin', cases);
  const short = port('fin', cases, (routine, c) => caseLine(routine, c).slice(0, -2 * TEXT_SLACK));
  const cut = port('fin', cases.map((c, i) => ({ ...c, text: c.text.slice(0, Math.max(1, full[i].used - 1)) })),
    (routine, c) => caseLine(routine, c).slice(0, -2 * TEXT_SLACK));
  full.forEach((r, i) => {
    assert.notEqual(r.result, 'more', `case ${i}: the full window ran out`);
    if (r.result === 'done' && r.used <= cases[i].text.length) assert.deepEqual(short[i], r, `case ${i}`);
    if (r.result === 'done' && r.used > 1) assert.equal(cut[i].result, 'more', `case ${i}`);
  });
});
