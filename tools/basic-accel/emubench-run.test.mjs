// SPDX-License-Identifier: MIT
// Runs Source/C64/EmuBench in mos6502.mjs on the real BASIC and KERNAL ROMs, against a model of
// IOH_Debug's emulator benchmark that writes the results a run gives, so the program's report
// is checked off the board: the figures, the bitmap check, and each way a run can fail.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { Mos6502, FLAG } from '../lib/mos6502.mjs';
import { commandExists } from '../lib/toolchain.mjs';
import { findRoms } from './emu-model.mjs';

const ROOT = path.resolve(import.meta.dirname, '../..');
const SOURCE = path.join(ROOT, 'Source/C64/EmuBench/source/EmuBench.asm');
const roms = findRoms();
const skip = (!commandExists('acme') && 'acme is not on PATH') || (!roms && 'no C64 ROMs (install VICE with its ROMs, or set C64_BASIC_ROM, C64_KERNAL_ROM and C64_CHARGEN_ROM)');

const CHROUT = 0xffd2;
const RETURN_TO = 0x0400;
const RESULTS = 0xc000;
const EXPECTED = 0xf4b7cd00;

function build() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'emubench-'));
  const prg = path.join(dir, 'EmuBench.prg');
  const labels = path.join(dir, 'labels');
  const built = spawnSync('acme', ['--vicelabels', labels, '--format', 'cbm', '--outfile', prg, path.basename(SOURCE)],
    { cwd: path.dirname(SOURCE), encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  const symbols = Object.fromEntries([...fs.readFileSync(labels, 'utf8').matchAll(/al C:([0-9a-f]+) \.(\w+)/gi)]
    .map((m) => [m[2], parseInt(m[1], 16)]));
  return { prg: fs.readFileSync(prg), symbols };
}

// IOH_Debug's struct FTBench
function block(r) {
  const b = Buffer.alloc(32);
  b.writeUInt8(r.status ?? 0, 0);
  b.writeUInt16LE(r.ms ?? 0, 2);
  b.writeUInt16LE(Math.floor(r.speed ?? 0), 4);
  b.writeUInt8(Math.round((r.speed ?? 0) * 100) % 100, 6);
  b.writeUInt8(Math.round((r.mips ?? 0) * 100) % 100, 7);
  b.writeUInt16LE(Math.floor(r.mips ?? 0), 8);
  b.writeUInt16LE(r.hooks ?? 0, 10);
  b.writeUInt32LE(r.bitmap ?? EXPECTED, 12);
  b.writeUInt16LE(0x3d56, 16);
  b.writeUInt16LE(0xc70a, 18);
  b.writeUInt16LE(r.stopPc ?? 0, 20);
  return b;
}

// runs[k] is what the TR gives run k: { trStatus } for a refused command, otherwise a block
function run({ prg, symbols }, runs, ident = 0x46) {
  const ram = new Uint8Array(0x10000);
  ram.set(prg.subarray(2), prg[0] | prg[1] << 8);
  const ft = new Uint8Array(16);
  ft[0x0f] = ident;
  let output = '';
  const cpu = new Mos6502({
    read(a) {
      if (a >= 0xa000 && a < 0xc000) return roms.basic[a - 0xa000];
      if (a >= 0xe000) return roms.kernal[a - 0xe000];
      if (a >= 0xdef0 && a <= 0xdeff) return ft[a - 0xdef0];
      return ram[a];
    },
    write(a, v) {
      if (a === 0xdef1) { ft[1] = v; return; }
      if (a === 0xdef0) {
        const k = ft[1] - 6;
        const r = runs[k];
        if (v !== 3 || !r) { ft[0] = 6; return; }
        if (r.trStatus) { ft[0] = r.trStatus; return; }
        ram.set(block(r), RESULTS + 32 * k);
        ft[0] = 2;
        return;
      }
      if (a < 0xd000 || a >= 0xe000) ram[a] = v;
    },
  });
  cpu.s = 0xff;
  cpu.pc = symbols.SysAddress;
  cpu.push16(RETURN_TO - 1);
  for (let i = 0; i < 5_000_000 && cpu.pc !== RETURN_TO; i++) {
    if (cpu.pc === CHROUT) {
      output += String.fromCharCode(cpu.a);
      cpu.p &= ~FLAG.C;
      cpu.pc = (cpu.pull16() + 1) & 0xffff;
      continue;
    }
    cpu.step();
  }
  assert.equal(cpu.pc, RETURN_TO, 'the program did not finish');
  return output.replace(/\r/g, '\n');
}

test('EmuBench reports both runs, the ROMs and the bitmap check', { skip }, () => {
  const out = run(build(), [
    { ms: 3456, speed: 21.05, mips: 5.9, hooks: 0 },
    { ms: 1234, speed: 19.5, mips: 5.07, hooks: 62884 },
  ]);
  if (process.env.SHOW_OUTPUT) console.log(out);
  assert.match(out, /ROMS READ: BASIC \$3D56 KERNAL \$C70A/);
  const [plain, hooked] = out.split(/FLOAT ROUTINES ANSWERED IN C:/);
  assert.match(plain, /RUN TO DONE, MS +3456\n X A 1 MHZ 6502 +21\.05\n MIPS +5\.90\n HOOK CALLS +0\n BITMAP +AS EXPECTED/);
  assert.match(hooked, /RUN TO DONE, MS +1234\n X A 1 MHZ 6502 +19\.50\n MIPS +5\.07\n HOOK CALLS +62884\n BITMAP +AS EXPECTED/);
});

test('EmuBench shows a bitmap that differs, a run that stopped, and a refused command', { skip }, () => {
  const out = run(build(), [
    { status: 2, stopPc: 0xa480 },
    { trStatus: 4 },
  ]);
  assert.match(out, /EMULATED:\n STOPPED AT \$A480/);
  assert.match(out, /ANSWERED IN C:\n TR STATUS +4/);
  const limits = run(build(), [{ status: 1, stopPc: 0xb8d2 }, { status: 3 }]);
  assert.match(limits, /EMULATED:\n TIME LIMIT AT \$B8D2/);
  assert.match(limits, /ANSWERED IN C:\n NO MEMORY FOR THE EMULATOR/);
  const differs = run(build(), [{ bitmap: 0x12345678 }, {}]);
  assert.match(differs, /EMULATED:[^]*BITMAP +DIFFERS: \$12345678/);
});

test('EmuBench says so when the Debug handler is not answering', { skip }, () => {
  assert.match(run(build(), [], 0xff), /NO EMULATOR BENCHMARK AT \$DEF0/);
});
