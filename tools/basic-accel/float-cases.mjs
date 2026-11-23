// SPDX-License-Identifier: MIT
// Test cases for the float routines in Source/Teensy/BasicAccel/CbmFloat.h, and the ROM's own
// routines run on them in mos6502.mjs. The ROM is not in this repository: findBasicRom() looks
// for the image VICE uses (basic-901226-01.bin), or the file C64_BASIC_ROM names.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { Mos6502, FLAG } from '../lib/mos6502.mjs';

export const BASIC_ROM_SHA1 = '79015323128650c742a3694c9429aa91f355905e';  // 901226-01

// Where each routine is entered. The T entries are where their routes in meet, with both
// operands in zero page; the others take a float in memory at A/Y and unpack it first.
export const ROUTINES = {
  faddt: 0xb86a, fsubt: 0xb853, fmultt: 0xba2b, fdivt: 0xbb12,
  fadd: 0xb867, fsub: 0xb850, fmult: 0xba28, fdiv: 0xbb0f,
  fin: 0xbcf3, qint: 0xbc9b, ayint: 0xb1bf,
};

const ERROR = 0xa437;  // every BASIC error, ?OVERFLOW and ?DIVISION BY ZERO among them
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

// Each case is { a, x, y, p, zp } as the routine is entered: A = $61 with N/Z from it, D clear.
// Mantissa bytes are often zero, since a zero byte takes MULSHF's path; exponents often sum to
// near 128, 256 or 384, where MULDIV underflows, zeroes or overflows, and often differ by
// little, which decides how far an add shifts one operand.
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
    else if (chance(3)) zp[0x69] = (zp[0x61] + (next() % 81) - 40) & 0xff;
    for (const at of [0x62, 0x63, 0x64, 0x65, 0x70, 0x6a, 0x6b, 0x6c, 0x6d]) zp[at] = mantissaByte();
    if (!chance(8)) zp[0x62] |= 0x80;
    if (!chance(8)) zp[0x6a] |= 0x80;
    if (chance(4)) for (let j = 0; j < 4; j++) zp[0x6a + j] = zp[0x62 + j];
    if (chance(2)) zp[0x68] = chance(2) ? 0 : 0xff;
    if (chance(2)) zp[0x6f] = (zp[0x66] ^ zp[0x6e]) & 0x80;
    const a = zp[0x61];
    let p = (byte() & (FLAG.C | FLAG.V | FLAG.I)) | FLAG.U;
    if (a === 0) p |= FLAG.Z;
    p |= a & FLAG.N;
    cases.push({ a, x: byte(), y: byte(), p, zp });
  }
  return cases;
}

// Cases for the entries that unpack a float from A/Y: makeCases', with ARG's value moved into
// memory at A/Y. Mostly RAM; sometimes BASIC's own zero-page temporaries, FAC or ARG itself
// (CONUPK then reads bytes it has just written), or a pointer whose last byte is in page 1.
const ZERO_PAGE_OPERANDS = [0x57, 0x5c, 0x61, 0x69, 0x6a, 0x26, 0xfc];
export function makeOperandCases(count, seed = 0x6502) {
  const next = random(seed ^ 0x5a5a5a5a);
  return makeCases(count, seed).map((c) => {
    const zp = c.zp.slice();
    const value = [zp[0x69], (zp[0x6e] & 0x80) | (zp[0x6a] & 0x7f), zp[0x6b], zp[0x6c], zp[0x6d]];
    for (const at of [0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f]) zp[at] = next() & 0xff;
    const ptr = next() % 4 === 0 ? ZERO_PAGE_OPERANDS[next() % ZERO_PAGE_OPERANDS.length] : 0x0200 + (next() % 0x9dfb);
    const page1 = [];
    value.forEach((v, i) => { if (ptr + i < 0x100) zp[ptr + i] = v; else page1.push([ptr + i, v]); });
    const mem = Uint8Array.from(value.map((v, i) => (ptr + i < 0x100 ? zp[ptr + i] : v)));
    return { a: ptr & 0xff, x: c.x, y: ptr >> 8, p: c.p, zp, mem, memAt: ptr };
  });
}

// Cases for QINT and AYINT: FAC near the integer limits they work to ($90 for AYINT, $A0 for
// QINT), now and then exactly -32768, which AYINT has to compare against its constant
export function makeIntCases(count, seed = 0x6502) {
  const next = random(seed);
  const byte = () => next() & 0xff;
  const chance = (n) => next() % n === 0;
  const cases = [];
  for (let i = 0; i < count; i++) {
    const zp = new Uint8Array(256);
    for (let j = 0; j < 256; j++) zp[j] = byte();
    zp[0x61] = chance(15) ? 0 : chance(4) ? [0x8f, 0x90, 0x91, 0x9f, 0xa0, 0xa1][next() % 6] : chance(5) ? byte() : 0x78 + (next() % 0x30);
    for (const at of [0x62, 0x63, 0x64, 0x65, 0x70]) zp[at] = chance(3) ? 0 : byte();
    if (!chance(8)) zp[0x62] |= 0x80;
    if (chance(10)) {
      zp[0x61] = 0x90; zp[0x62] = 0x80; zp[0x63] = 0; zp[0x64] = 0; zp[0x65] = 0; zp[0x66] |= 0x80;
      zp[0x70] = chance(2) ? byte() & 0x7f : byte();
    }
    if (chance(2)) zp[0x68] = chance(2) ? 0 : 0xff;
    cases.push({ a: byte(), x: byte(), y: byte(), p: (byte() & ~FLAG.D & ~FLAG.B) | FLAG.U, zp });
  }
  return cases;
}

// The ROM's CHRGET, as the KERNAL copies it to $73; bytes 7 and 8 are TXTPTR
export const CHRGET = [
  0xe6, 0x7a, 0xd0, 0x02, 0xe6, 0x7b, 0xad, 0x00, 0x00, 0xc9, 0x3a, 0xb0,
  0x0a, 0xc9, 0x20, 0xf0, 0xef, 0x38, 0xe9, 0x30, 0x38, 0xe9, 0xd0, 0x60,
];
const stockChrget = (zp) => CHRGET.every((b, i) => i === 7 || i === 8 || zp[0x73 + i] === b);

const TERMINATORS = [0x00, 0x3a, 0x2c, 0x29, 0xac, 0xaa, 0xab, 0x41, 0x58, 0xff, 0x22, 0x3b];

// FIN cases: the text of a number at TXTPTR, as BASIC, INPUT, READ or VAL leave it: signs, spaces
// (CHRGET skips them), a point, an E with a sign as a character or a token, and a terminator.
// A, and C clear for a digit, are what CHRGET left; CHRGET in zero page is the ROM's, except
// sometimes, when FIN must hand back to the ROM.
export function makeFinCases(count, seed = 0x6502) {
  const next = random(seed);
  const byte = () => next() & 0xff;
  const chance = (n) => next() % n === 0;
  const cases = [];
  for (let i = 0; i < count; i++) {
    const text = [];
    const digits = (n) => {
      for (let j = 0; j < n; j++) {
        text.push(0x30 + (next() % 10));
        if (chance(12)) text.push(0x20);
      }
    };
    if (chance(4)) text.push(chance(2) ? 0x2d : 0x2b);
    digits(chance(6) ? next() % 14 : next() % 6);
    if (chance(2)) {
      text.push(0x2e);
      digits(next() % 9);
      if (chance(15)) { text.push(0x2e); digits(next() % 3); }
    }
    if (chance(3)) {
      text.push(0x45);
      if (!chance(3)) text.push([0x2d, 0x2b, 0xab, 0xaa][next() % 4]);
      digits(chance(5) ? next() % 4 : next() % 3);
    }
    text.push(TERMINATORS[next() % TERMINATORS.length]);
    for (let j = 0; j < 4; j++) text.push(byte());
    text.push(0);
    if (text[0] === 0x20) text[0] = 0x31;
    const textAt = 0x0800 + (next() % 0x8800);
    const zp = new Uint8Array(256);
    for (let j = 0; j < 256; j++) zp[j] = byte();
    CHRGET.forEach((b, j) => { zp[0x73 + j] = b; });
    zp[0x7a] = textAt & 0xff;
    zp[0x7b] = textAt >> 8;
    if (chance(40)) zp[0x73 + [0, 1, 6, 10, 14, 19, 23][next() % 7]] ^= 1 << (next() % 8);
    if (chance(2)) zp[0x68] = chance(2) ? 0 : 0xff;
    const a = text[0];
    let p = (byte() & (FLAG.V | FLAG.I | FLAG.N | FLAG.Z)) | FLAG.U;
    if (!(a >= 0x30 && a <= 0x39)) p |= FLAG.C;
    cases.push({ a, x: byte(), y: byte(), p, zp, text: Uint8Array.from(text), textAt });
  }
  return cases;
}

const OPERAND_ENTRIES = new Set(['fadd', 'fsub', 'fmult', 'fdiv']);
export function casesFor(routine, count, seed) {
  if (routine === 'fin') return makeFinCases(count, seed);
  if (routine === 'qint' || routine === 'ayint') return makeIntCases(count, seed);
  if (OPERAND_ENTRIES.has(routine)) return makeOperandCases(count, seed);
  return makeCases(count, seed);
}

// Runs the ROM's routine on one case. result is 'done' when it returned, or 'native' when it
// raised a BASIC error, which the ports hand back to the ROM; FIN also hands back when CHRGET
// is not the ROM's, so the ROM is not run then.
export function runRom(rom, routine, c) {
  if (routine === 'fin' && !stockChrget(c.zp)) return { result: 'native' };
  const ram = new Uint8Array(0x10000);
  ram.set(c.zp, 0);
  if (c.text) ram.set(c.text, c.textAt);
  if (c.mem) c.mem.forEach((v, i) => { if (c.memAt + i >= 0x100) ram[c.memAt + i] = v; });
  const cpu = new Mos6502({
    read: (a) => (a >= 0xa000 && a < 0xc000 ? rom[a - 0xa000] : ram[a]),
    write: (a, v) => { ram[a] = v; },
  });
  Object.assign(cpu, { a: c.a, x: c.x, y: c.y, p: c.p, s: 0xff, pc: ROUTINES[routine] });
  cpu.push16(RETURN_TO - 1);
  cpu.run((cpu) => cpu.pc === RETURN_TO || cpu.pc === ERROR, 5_000_000);
  const result = cpu.pc === ERROR ? 'native' : 'done';
  return { result, a: cpu.a, x: cpu.x, y: cpu.y, p: cpu.p, zp: ram.slice(0, 256) };
}

// FIN's text window as the port gets it: the case's text, and zeros after it as RAM holds them
export const TEXT_SLACK = 16;
const hex = (bytes) => Buffer.from(bytes).toString('hex');
export const caseLine = (routine, c) =>
  `${routine} ${hex([c.a])} ${hex([c.x])} ${hex([c.y])} ${hex([c.p])} ${hex(c.zp)}` +
  (c.text ? ` ${hex(c.text)}${'00'.repeat(TEXT_SLACK)}` : c.mem ? ` ${hex(c.mem)}` : '');

// A result as the vectors file lists it: the registers and the zero page the routines write, or
// only 'native', since what the ROM then does is the ROM's
export const OUTPUT_ZP = [
  0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x56, 0x5d, 0x5e, 0x5f, 0x60,
  0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x7a, 0x7b,
];
export const resultLine = (r) =>
  r.result === 'native' ? 'native' : `done ${hex([r.a, r.x, r.y, r.p])} ${hex(OUTPUT_ZP.map((at) => r.zp[at]))}`;
