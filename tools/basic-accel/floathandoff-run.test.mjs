// SPDX-License-Identifier: MIT
// Runs Source/C64/FloatHandoff in mos6502.mjs on the real BASIC and KERNAL ROMs, against a model
// of IOH_Debug's float hand-off test that answers with the C port as the firmware does: from the
// zero-page spans it reads by DMA, written back the same way. That checks the program's logic,
// and its comparison with the ROM, before it meets a board; the timings it prints here mean
// nothing, since the model's timer counts instructions.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { Mos6502, FLAG } from '../lib/mos6502.mjs';
import { commandExists } from '../lib/toolchain.mjs';
import { findBasicRom } from './float-cases.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const SOURCE = path.join(root, 'Source/C64/FloatHandoff/source/FloatHandoff.asm');
const HARNESS_SOURCE = path.join(root, 'Source/Teensy/tests/cbm_float_harness.c');
const KERNAL = '/usr/share/vice/C64/kernal-901227-03.bin';

const basic = findBasicRom();
const skip = (!commandExists('acme') && 'acme is not on PATH') || (!commandExists('cc') && 'cc is not on PATH') ||
  (!basic && 'no C64 BASIC ROM') || (!fs.existsSync(KERNAL) && `no KERNAL ROM at ${KERNAL}`);

const ROUTINES = ['faddt', 'fsubt', 'fmultt', 'fdivt', 'qint', 'ayint'];
const SPANS = [[0x22, 9], [0x56, 27]];
const CHROUT = 0xffd2;
const RETURN_TO = 0x0400;

function build() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'floathandoff-'));
  const prg = path.join(dir, 'FloatHandoff.prg');
  const labels = path.join(dir, 'labels');
  const built = spawnSync('acme', ['--vicelabels', labels, '--format', 'cbm', '--outfile', prg, path.basename(SOURCE)],
    { cwd: path.dirname(SOURCE), encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  const harness = path.join(dir, 'harness');
  const cc = spawnSync('cc', ['-std=c99', '-O2', '-o', harness, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(cc.status, 0, cc.stderr);
  const symbols = Object.fromEntries([...fs.readFileSync(labels, 'utf8').matchAll(/al C:([0-9a-f]+) \.(\w+)/gi)]
    .map((m) => [m[2], parseInt(m[1], 16)]));
  return { prg: fs.readFileSync(prg), symbols, harness };
}

// corrupt(routine, zp) may change what the TR writes back, to show the program notices
function run({ prg, symbols, harness }, corrupt = () => {}) {
  const kernal = fs.readFileSync(KERNAL);
  const ram = new Uint8Array(0x10000);
  const load = prg[0] | prg[1] << 8;
  ram.set(prg.subarray(2), load);
  const ft = new Uint8Array(16);
  ft[0x0f] = 0x46;
  let routine = 0;
  let steps = 0;
  let timerStart = 0;
  let timerStop = 0;
  let raster = 0;
  let output = '';

  const answer = () => {
    const zp = new Uint8Array(256);
    for (const [first, len] of SPANS) zp.set(ram.subarray(first, first + len), first);
    const a = zp[0x61];
    const p = (a ? 0 : FLAG.Z) | (a & FLAG.N) | FLAG.U;
    const hex = (v) => v.toString(16).padStart(2, '0');
    const line = `${ROUTINES[routine]} ${hex(a)} 00 00 ${hex(p)} ${Buffer.from(zp).toString('hex')}\n`;
    const out = spawnSync(harness, [], { input: line, encoding: 'utf8' });
    const [result, set, A, X, Y, P, z] = out.stdout.trim().split(' ');
    if (result !== 'done') return 3;
    const after = Buffer.from(z, 'hex');
    corrupt(ROUTINES[routine], after);
    for (const [first, len] of SPANS) ram.set(after.subarray(first, first + len), first);
    ft.set([A, X, Y, P].map((h) => parseInt(h, 16)), 2);
    ft[6] = parseInt(set, 16) & 0xff;
    ft[7] = parseInt(set, 16) >> 8;
    return 2;
  };

  const cpu = new Mos6502({
    read(a) {
      if (a >= 0xa000 && a < 0xc000) return basic[a - 0xa000];
      if (a >= 0xe000) return kernal[a - 0xe000];
      if (a === 0xd011) return (raster++ & 1) ? 0x80 : 0x1b;
      if (a === 0xdd04 || a === 0xdd05) {
        const left = 0xffff - (((timerStop || steps) - timerStart) * 3 & 0xffff);
        return a === 0xdd04 ? left & 0xff : left >> 8;
      }
      if (a >= 0xdef0 && a <= 0xdeff) return ft[a - 0xdef0];
      return ram[a];
    },
    write(a, v) {
      if (a === 0xdd0e) {
        if (v & 1) { timerStart = steps; timerStop = 0; }
        else if (!timerStop) timerStop = steps;
        return;
      }
      if (a === 0xdef1) { routine = v; ft[1] = v; return; }
      if (a === 0xdef0) { ft[0] = v === 3 ? answer() : 2; return; }
      if (a < 0xa000 || (a >= 0xc000 && a < 0xd000)) ram[a] = v;
    },
  });
  cpu.s = 0xff;
  cpu.pc = symbols.SysAddress;
  cpu.push16(RETURN_TO - 1);
  for (let i = 0; i < 50_000_000 && cpu.pc !== RETURN_TO; i++) {
    if (cpu.pc === CHROUT) {
      output += String.fromCharCode(cpu.a);
      cpu.p &= ~FLAG.C;  // CHROUT returns carry clear; BASIC's output ($E10C) takes it as an I/O error
      cpu.pc = (cpu.pull16() + 1) & 0xffff;
      continue;
    }
    cpu.step();
    steps++;
  }
  assert.equal(cpu.pc, RETURN_TO, 'the program did not finish');
  const errors = [...ram.subarray(symbols.Errors, symbols.Errors + 6)];
  return { errors, pingErrors: ram[symbols.PingErrors], output };
}

test('FloatHandoff finds the TR\'s answers the same as the ROM\'s', { skip }, () => {
  const r = run(build());
  if (process.env.SHOW_OUTPUT) console.log(r.output.replace(/\r/g, '\n'));
  assert.deepEqual(r.errors, [0, 0, 0, 0, 0, 0], r.output);
  assert.equal(r.pingErrors, 0);
  assert.match(r.output, /FMULTT/i);
});

test('FloatHandoff counts an answer that differs from the ROM\'s', { skip }, () => {
  const r = run(build(), (routine, zp) => { if (routine === 'fmultt') zp[0x65] ^= 1; });
  assert.deepEqual(r.errors, [0, 0, 255, 0, 0, 0]);
});
