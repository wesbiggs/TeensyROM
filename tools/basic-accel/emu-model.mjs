// SPDX-License-Identifier: MIT
// Models running BASIC on the TR+ with Source/Teensy/BasicAccel/BasicEmu.h: the ROMs run on the
// Teensy against a copy of the C64's RAM, the float routines answered by CbmFloat.h, and only I/O
// and what the VIC displays crossing the bus. Runs a listing natively and in each modeled
// configuration, and prints the times and the bus traffic.
//
//   node tools/basic-accel/emu-model.mjs [--program FILE.bas] [--speed 10,20,40] [--read 3,75]
//                                        [--write N] [--hook N] [--json]
//
// Needs cc, and the BASIC, KERNAL and character ROMs VICE ships (or C64_BASIC_ROM,
// C64_KERNAL_ROM and C64_CHARGEN_ROM). See README.md, "Running the ROM on the Teensy".
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { findBasicRom, findC64Rom } from './float-cases.mjs';
import { tokenize } from './basic-tokenize.mjs';

const HERE = import.meta.dirname;
const MODEL_SOURCE = path.join(HERE, 'emu_model.c');
export const SINE_PLOT = path.join(HERE, 'SinePlot.bas');
export const ALL_HOOKS = (1 << 11) - 1;

export function findRoms() {
  const basic = findBasicRom();
  const kernal = findC64Rom('kernal-901227-03.bin', '1d503e56df85a62fee696e7618dc5b4e781df1bb', 'C64_KERNAL_ROM', 0x2000);
  const chargen = findC64Rom('chargen-901225-01.bin', 'adc7c31e18c7c7413d54802ef2f4193da14711aa', 'C64_CHARGEN_ROM', 0x1000);
  return basic && kernal && chargen ? { basic, kernal, chargen } : null;
}

// Builds the model and writes the ROMs and the program beside it; returns a function that runs it
export function prepare(roms, listing) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'emu-model-'));
  const exe = path.join(dir, 'emu_model');
  const built = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe, MODEL_SOURCE], { encoding: 'utf8' });
  if (built.status !== 0) throw new Error(`cc: ${built.stderr}`);
  const files = ['basic', 'kernal', 'chargen'].map((name) => {
    const file = path.join(dir, `${name}.bin`);
    fs.writeFileSync(file, roms[name]);
    return file;
  });
  const prg = path.join(dir, 'program.prg');
  fs.writeFileSync(prg, tokenize(listing));
  return (options = {}) => {
    const args = [...files, prg];
    if (options.native) args.push('--native');
    for (const key of ['speed', 'read', 'write', 'hook', 'hooks']) if (options[key] !== undefined) args.push(`--${key}`, String(options[key]));
    const ran = spawnSync(exe, args, { encoding: 'utf8' });
    if (ran.status !== 0) throw new Error(`emu_model ${args.slice(4).join(' ')}: ${ran.stderr}`);
    return JSON.parse(ran.stdout);
  };
}

const list = (text) => text.split(',').map(Number);

function main(argv) {
  const opts = { program: SINE_PLOT, speed: [10, 20, 40], read: [3, 75], write: 1, hook: 2, json: false };
  for (let i = 0; i < argv.length; i++) {
    const [flag, value] = [argv[i], argv[i + 1]];
    if (flag === '--json') opts.json = true;
    else if (flag === '--program') opts.program = argv[++i];
    else if (flag === '--speed' || flag === '--read') { opts[flag.slice(2)] = list(value); i++; }
    else if (flag === '--write' || flag === '--hook') { opts[flag.slice(2)] = Number(value); i++; }
    else {
      console.error('Usage: node tools/basic-accel/emu-model.mjs [--program FILE.bas] [--speed 10,20,40] [--read 3,75] [--write N] [--hook N] [--json]');
      process.exit(2);
    }
  }
  const roms = findRoms();
  if (!roms) {
    console.error('needs the C64 BASIC, KERNAL and character ROMs: install VICE with its ROMs, or set C64_BASIC_ROM, C64_KERNAL_ROM and C64_CHARGEN_ROM');
    process.exit(2);
  }
  const run = prepare(roms, fs.readFileSync(opts.program, 'utf8'));
  const native = run({ native: true });
  const runs = [{ label: 'native', ...native }];
  for (const speed of opts.speed) {
    for (const read of opts.read) {
      runs.push({ label: `${speed}x, read ${read}, no hooks`, ...run({ speed, read, write: opts.write, hooks: 0 }) });
      runs.push({ label: `${speed}x, read ${read}, hooks`, ...run({ speed, read, write: opts.write, hook: opts.hook }) });
    }
  }
  if (opts.json) {
    console.log(JSON.stringify(runs, null, 2));
    return;
  }
  const seconds = (r) => r.runCycles / 985248;
  console.log('| Run | RUN to done | Speedup | Program\'s TI | Bus reads | Bus writes | Of them flushed | Same bitmap |');
  console.log('|---|---|---|---|---|---|---|---|');
  for (const r of runs) {
    console.log(`| ${r.label} | ${seconds(r).toFixed(2)} s | ${(native.runCycles / r.runCycles).toFixed(1)}x | ${r.jiffies} | ` +
      `${r.busReads} | ${r.busWrites} | ${r.flushed} | ${r.bitmap === native.bitmap ? 'yes' : 'NO'} |`);
  }
  const hooked = runs.find((r) => r.hooks);
  if (hooked) {
    console.log(`\n6502 work from RUN: ${native.instructions} instructions natively, ${hooked.instructions} with the hooks ` +
      `(${(hooked.cpuCycles / native.cpuCycles * 100).toFixed(0)}% of the cycles); hook calls ${hooked.hookCalls.reduce((a, b) => a + b, 0)}, ` +
      `handed back to the ROM ${hooked.hookNative.reduce((a, b) => a + b, 0)}`);
  }
}

if (process.argv[1] === import.meta.filename) main(process.argv.slice(2));
