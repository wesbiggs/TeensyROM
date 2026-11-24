// SPDX-License-Identifier: MIT
// Cpu6502.h against tools/lib/mos6502.mjs, one instruction at a time from the same state, and
// its cycle counts against the NMOS table.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';
import { Mos6502, OPCODES, FLAG } from '../../../tools/lib/mos6502.mjs';

const HARNESS_SOURCE = path.join(import.meta.dirname, 'cpu6502_harness.c');
const CASES_PER_OPCODE = 300;

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('cc') && !inCi && 'cc is not on PATH';

// Cycles per documented opcode on the NMOS part, before the page-cross and branch extras
const BASE_CYCLES = `
7 6 - - - 3 5 - 3 2 2 - - 4 6 -
2 5 - - - 4 6 - 2 4 - - - 4 7 -
6 6 - - 3 3 5 - 4 2 2 - 4 4 6 -
2 5 - - - 4 6 - 2 4 - - - 4 7 -
6 6 - - - 3 5 - 3 2 2 - 3 4 6 -
2 5 - - - 4 6 - 2 4 - - - 4 7 -
6 6 - - - 3 5 - 4 2 2 - 5 4 6 -
2 5 - - - 4 6 - 2 4 - - - 4 7 -
- 6 - - 3 3 3 - 2 - 2 - 4 4 4 -
2 6 - - 4 4 4 - 2 5 2 - - 5 - -
2 6 2 - 3 3 3 - 2 2 2 - 4 4 4 -
2 5 - - 4 4 4 - 2 4 2 - 4 4 4 -
2 6 - - 3 3 5 - 2 2 2 - 4 4 6 -
2 5 - - - 4 6 - 2 4 - - - 4 7 -
2 6 - - 3 3 5 - 2 2 2 - 4 4 6 -
2 5 - - - 4 6 - 2 4 - - - 4 7 -
`.trim().split(/\s+/).map((c) => (c === '-' ? null : Number(c)));

const JAMS = [0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xb2, 0xd2, 0xf2];
const READS = new Set(['ADC', 'AND', 'CMP', 'EOR', 'LDA', 'LDX', 'LDY', 'ORA', 'SBC']);
const MODIFIES = new Set(['ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC']);
// a branch with offset 0 is taken and still lands on the next instruction, so take comes from P
const BRANCHES = {
  BCC: (p) => !(p & FLAG.C), BCS: (p) => p & FLAG.C, BNE: (p) => !(p & FLAG.Z), BEQ: (p) => p & FLAG.Z,
  BPL: (p) => !(p & FLAG.N), BMI: (p) => p & FLAG.N, BVC: (p) => !(p & FLAG.V), BVS: (p) => p & FLAG.V,
};
const OPERAND_BYTES = { imp: 0, acc: 0, imm: 1, rel: 1, zp: 1, zpx: 1, zpy: 1, indx: 1, indy: 1, abs: 2, absx: 2, absy: 2, ind: 2 };

// the same as Cpu6502TestByte in the harness
function testByte(seed, address) {
  let h = (Math.imul(address, 0x9e3779b1) ^ seed) >>> 0;
  h = (h ^ (h >>> 15)) >>> 0;
  h = Math.imul(h, 0x85ebca77) >>> 0;
  h = (h ^ (h >>> 13)) >>> 0;
  return h & 0xff;
}

function random(seed) {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s;
  };
}

const hex = (v, digits = 2) => v.toString(16).padStart(digits, '0');

// A case is a start state plus the bytes it sets; any other byte is testByte(seed, address).
function makeCases() {
  const next = random(0x6502);
  const cases = [];
  // operand bytes that land near page and zero-page ends, so crossings and wraps are common
  const edgy = () => [0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff][next() % 6];
  for (const [opcode, { mode }] of OPCODES) {
    for (let i = 0; i < CASES_PER_OPCODE; i++) {
      const pc = i % 50 === 0 ? 0xfffe - (i % 3) : next() & 0xffff;
      const c = {
        seed: next(), pc, a: next() & 0xff, x: next() & 0xff, y: next() & 0xff, s: next() & 0xff,
        p: (next() & 0xff & ~FLAG.B) | FLAG.U, irq: 0, nmi: 0, bytes: new Map([[pc, opcode]]),
      };
      for (let b = 1; b <= OPERAND_BYTES[mode]; b++) c.bytes.set((pc + b) & 0xffff, next() % 2 ? edgy() : next() & 0xff);
      if (mode === 'indy' || mode === 'indx') {
        // the pointer itself, near a page end, so the indexing crosses
        const z = mode === 'indy' ? c.bytes.get((pc + 1) & 0xffff) : (c.bytes.get((pc + 1) & 0xffff) + c.x) & 0xff;
        if (!c.bytes.has(z)) c.bytes.set(z, next() % 2 ? edgy() : next() & 0xff);
      }
      cases.push(c);
    }
  }
  // an interrupt taken before the instruction: the instruction is then the one at the vector
  for (let i = 0; i < 600; i++) {
    const nmi = i % 2;
    const vector = nmi ? 0xfffa : 0xfffe;
    const to = 0x2000 + (next() & 0x7fff);
    const opcodes = [...OPCODES.keys()];
    const op = opcodes[next() % opcodes.length];
    const pc = next() & 0xffff;
    // an IRQ with I set is not taken, and the instruction at PC runs instead
    const c = {
      seed: next(), pc, a: next() & 0xff, x: next() & 0xff, y: next() & 0xff, s: next() & 0xff,
      p: (next() & 0xff & ~FLAG.B) | FLAG.U, irq: nmi ? next() & 1 : 1, nmi,
      bytes: new Map([[pc, op], [vector, to & 0xff], [vector + 1, to >> 8], [to, op]]),
    };
    cases.push(c);
  }
  return cases;
}

const line = (c) =>
  [c.seed, c.pc, c.a, c.x, c.y, c.s, c.p, c.irq, c.nmi].map((v) => v.toString(16)).join(' ') +
  [...c.bytes].map(([at, v]) => ` ${hex(at, 4)}:${hex(v)}`).join('');

// mos6502.mjs on the same state; it has no cycle counts, so they come from the table
function reference(c) {
  const mem = new Map(c.bytes);
  const writes = [];
  const read = (at) => (mem.has(at) ? mem.get(at) : testByte(c.seed, at));
  const cpu = new Mos6502({ read, write: (at, v) => { mem.set(at, v); writes.push([at, v]); } });
  Object.assign(cpu, { pc: c.pc, a: c.a, x: c.x, y: c.y, s: c.s, p: c.p });
  let cycles = 0;
  if (c.nmi) { cpu.nmi(); cycles += 7; } else if (c.irq && !(c.p & FLAG.I)) { cpu.irq(); cycles += 7; }
  const at = cpu.pc;
  const opcode = read(at);
  const { mnemonic, mode } = OPCODES.get(opcode);
  const operand = read((at + 1) & 0xffff);
  const operand16 = operand | (read((at + 2) & 0xffff) << 8);
  const before = { x: cpu.x, y: cpu.y, p: cpu.p };
  const modified = MODIFIES.has(mnemonic) && mode !== 'acc' ? read(address(mode)) : null;
  cpu.step();
  cycles += BASE_CYCLES[opcode];
  if (READS.has(mnemonic)) {
    let base = null;
    let index = 0;
    if (mode === 'absx') [base, index] = [operand16, before.x];
    if (mode === 'absy') [base, index] = [operand16, before.y];
    if (mode === 'indy') [base, index] = [read(operand) | (read((operand + 1) & 0xff) << 8), before.y];
    if (base !== null && ((base + index) & 0xff00) !== (base & 0xff00)) cycles++;
  }
  if (BRANCHES[mnemonic]?.(before.p)) {
    const fallThrough = (at + 2) & 0xffff;
    cycles += (cpu.pc & 0xff00) === (fallThrough & 0xff00) ? 1 : 2;
  }
  return { cpu, cycles, writes, modified };

  function address(m) {
    if (m === 'zp') return operand;
    if (m === 'zpx') return (operand + cpu.x) & 0xff;
    if (m === 'abs') return operand16;
    return (operand16 + cpu.x) & 0xffff;
  }
}

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cpu6502-'));
  harnessPath = path.join(dir, 'cpu6502_harness');
  const built = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function run(cases) {
  const ran = spawnSync(harness(), [], { input: cases.map(line).join('\n') + '\n', encoding: 'utf8', maxBuffer: 1 << 28 });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.trim().split('\n').map((out) => {
    const [stop, pc, a, x, y, s, p, cycles, ...writes] = out.split(' ');
    return {
      stop, pc: parseInt(pc, 16), a: parseInt(a, 16), x: parseInt(x, 16), y: parseInt(y, 16), s: parseInt(s, 16),
      p: parseInt(p, 16), cycles: Number(cycles), writes: writes.map((w) => w.split(':').map((v) => parseInt(v, 16))),
    };
  });
}

test('each documented opcode matches mos6502.mjs, its writes included', { skip }, () => {
  const cases = makeCases();
  const results = run(cases);
  assert.equal(results.length, cases.length);
  let failures = 0;
  cases.forEach((c, i) => {
    const want = reference(c);
    const got = results[i];
    const { cpu } = want;
    // a read-modify-write on memory writes its operand back unchanged before the result
    const last = want.writes.at(-1);
    const wantWrites = want.modified === null ? want.writes : [...want.writes.slice(0, -1), [last[0], want.modified], last];
    const same = got.stop === 'ran' && got.pc === cpu.pc && got.a === cpu.a && got.x === cpu.x && got.y === cpu.y &&
      got.s === cpu.s && got.p === cpu.p && got.cycles === want.cycles &&
      JSON.stringify(got.writes) === JSON.stringify(wantWrites);
    if (!same && failures++ < 10) {
      console.error(`case ${line(c)}\n  got  ${JSON.stringify(got)}\n  want ${JSON.stringify({ pc: cpu.pc, a: cpu.a, x: cpu.x, y: cpu.y, s: cpu.s, p: cpu.p, cycles: want.cycles, writes: wantWrites })}`);
    }
  });
  assert.equal(failures, 0, `${failures} of ${cases.length} cases differ`);
});

test('JAM opcodes stop on themselves, and other undocumented opcodes stop as undefined', { skip }, () => {
  const cases = [];
  for (let op = 0; op < 256; op++) {
    if (OPCODES.has(op)) continue;
    cases.push({ seed: op, pc: 0x1234, a: 1, x: 2, y: 3, s: 0xf0, p: FLAG.U, irq: 0, nmi: 0, bytes: new Map([[0x1234, op]]) });
  }
  const results = run(cases);
  cases.forEach((c, i) => {
    const op = c.bytes.get(0x1234);
    const got = results[i];
    assert.equal(got.stop, JAMS.includes(op) ? 'jam' : 'undefined', `opcode ${hex(op)}`);
    assert.deepEqual([got.pc, got.a, got.x, got.y, got.s, got.cycles, got.writes.length], [0x1234, 1, 2, 3, 0xf0, 0, 0], `opcode ${hex(op)}`);
  });
});
