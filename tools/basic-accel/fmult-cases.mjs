// SPDX-License-Identifier: MIT
// FMULTT ($BA2B) test cases, and the ROM's own FMULTT run on them in mos6502.mjs, for checking
// Source/Teensy/BasicAccel/CbmFMult.h. The ROM is not in this repository: findBasicRom() looks
// for the image VICE uses (basic-901226-01.bin), or the file C64_BASIC_ROM names.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { Mos6502, FLAG } from '../lib/mos6502.mjs';

export const BASIC_ROM_SHA1 = '79015323128650c742a3694c9429aa91f355905e';  // 901226-01
export const FMULTT = 0xba2b;
const OVERFLOW_ERROR = 0xb97e;
const RETURN_TO = 0x0400;

export function findBasicRom() {
  const candidates = [
    process.env.C64_BASIC_ROM,
    '/usr/share/vice/C64/basic-901226-01.bin',
    '/usr/lib/vice/C64/basic-901226-01.bin',
    '/usr/local/share/vice/C64/basic-901226-01.bin',
    '/opt/homebrew/share/vice/C64/basic-901226-01.bin',
    path.join(os.homedir(), '.local/share/vice/C64/basic-901226-01.bin'),
  ].filter(Boolean);
  for (const file of candidates) {
    if (!fs.existsSync(file)) continue;
    const rom = fs.readFileSync(file);
    if (rom.length === 0x2000 && createHash('sha1').update(rom).digest('hex') === BASIC_ROM_SHA1) return rom;
  }
  return null;
}

// xorshift32, so a case list is the same on every host
function random(seed) {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s;
  };
}

const EXPONENTS = [0x00, 0x01, 0x02, 0x3f, 0x40, 0x41, 0x7f, 0x80, 0x81, 0xbf, 0xc0, 0xc1, 0xfe, 0xff];

// Each case is { a, x, y, p, zp } as FMULTT is entered: A = $61 with N/Z from it, D clear.
// Mantissa bytes are often zero, since a zero multiplier byte takes MULSHF's path, and the
// exponents often sum to near 128, 256 or 384, where MULDIV underflows, zeroes or overflows.
export function makeCases(count, seed = 0x6502) {
  const next = random(seed);
  const byte = () => next() & 0xff;
  const chance = (n) => next() % n === 0;
  const mantissaByte = () => (chance(3) ? 0 : byte());
  const exponent = () => (chance(3) ? EXPONENTS[next() % EXPONENTS.length] : byte());
  const cases = [];
  for (let i = 0; i < count; i++) {
    const zp = new Uint8Array(256);
    for (let j = 0; j < 256; j++) zp[j] = byte();
    zp[0x61] = chance(20) ? 0 : exponent();
    zp[0x69] = chance(20) ? 0 : exponent();
    if (chance(4)) zp[0x69] = (0x80 + (next() % 5) - 2 - zp[0x61] + (next() % 3) * 0x80) & 0xff;
    for (const at of [0x62, 0x63, 0x64, 0x65, 0x70, 0x6a, 0x6b, 0x6c, 0x6d]) zp[at] = mantissaByte();
    if (!chance(8)) zp[0x62] |= 0x80;
    if (!chance(8)) zp[0x6a] |= 0x80;
    if (chance(2)) zp[0x68] = chance(2) ? 0 : 0xff;
    const a = zp[0x61];
    let p = (byte() & (FLAG.C | FLAG.V | FLAG.I)) | FLAG.U;
    if (a === 0) p |= FLAG.Z;
    p |= a & FLAG.N;
    cases.push({ a, x: byte(), y: byte(), p, zp });
  }
  return cases;
}

// Runs the ROM's FMULTT on one case. result is 'done' when it returned, or 'native' for the two
// outcomes CbmFMultT hands back to the ROM: FAC was zero (it returns at once) or ?OVERFLOW ERROR.
export function runRom(rom, c) {
  const ram = new Uint8Array(0x10000);
  ram.set(c.zp, 0);
  const cpu = new Mos6502({
    read: (a) => (a >= 0xa000 && a < 0xc000 ? rom[a - 0xa000] : ram[a]),
    write: (a, v) => { ram[a] = v; },
  });
  Object.assign(cpu, { a: c.a, x: c.x, y: c.y, p: c.p, s: 0xff, pc: FMULTT });
  cpu.push16(RETURN_TO - 1);
  cpu.run((cpu) => cpu.pc === RETURN_TO || cpu.pc === OVERFLOW_ERROR, 100_000);
  const result = cpu.pc === OVERFLOW_ERROR || c.zp[0x61] === 0 ? 'native' : 'done';
  return { result, a: cpu.a, x: cpu.x, y: cpu.y, p: cpu.p, zp: ram.slice(0, 256) };
}

const hex = (bytes) => Buffer.from(bytes).toString('hex');
export const caseLine = (c) => `${hex([c.a, c.x, c.y, c.p])} ${hex(c.zp)}`.replace(/^(..)(..)(..)(..)/, '$1 $2 $3 $4');

// A result as the vectors file lists it: the registers and the zero page FMULTT writes, or only
// 'native', since what the ROM then does is the ROM's
export const OUTPUT_ZP = [0x26, 0x27, 0x28, 0x29, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x70];
export const resultLine = (r) =>
  r.result === 'native' ? 'native' : `done ${hex([r.a, r.x, r.y, r.p])} ${hex(OUTPUT_ZP.map((at) => r.zp[at]))}`;
