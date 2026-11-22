// SPDX-License-Identifier: MIT
// Runs TlsTerm's receive path (Source/C64/WebGet/tlsterm.a) against a model of
// the SwiftLink, and checks what reaches CHROUT.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { Mos6502 } from '../lib/mos6502.mjs';
import { commandExists } from '../lib/toolchain.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');

const CHROUT = 0xffd2;
const SENTINEL = 0x0200;
const ST_DCD = 0x40;     // high while no carrier
const ST_RXFULL = 0x08;

function assemble() {
  const out = fs.mkdtempSync(path.join(os.tmpdir(), 'tlsterm-test-'));
  try {
    const prg = path.join(out, 'tlsterm.prg');
    const labels = path.join(out, 'labels');
    const r = spawnSync('acme', ['-f', 'cbm', '-o', prg, '--vicelabels', labels,
      path.join(root, 'Source/C64/WebGet/tlsterm.a')], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    const L = Object.fromEntries([...fs.readFileSync(labels, 'utf8').matchAll(/^al C:([0-9a-f]+) \.(\w+)$/gim)]
      .map((m) => [m[2], parseInt(m[1], 16)]));
    return { bytes: fs.readFileSync(prg), L };
  } finally {
    fs.rmSync(out, { recursive: true, force: true });
  }
}

class Term {
  constructor({ bytes, L }) {
    this.L = L;
    this.ram = new Uint8Array(0x10000);
    this.ram.set(bytes.subarray(2), bytes[0] | (bytes[1] << 8));
    this.ram[CHROUT] = 0x60;   // rts: the test reads A as the CPU reaches it
    this.status = 0x10 | ST_DCD;
    this.data = 0;
    this.cmd = 0;
    this.printed = [];
    this.cpu = new Mos6502({
      read: (a) => {
        if (a === L.sw_data) { this.status &= ~ST_RXFULL; return this.data; }
        if (a === L.sw_stat) return this.status;
        if (a === L.sw_cmd) return this.cmd;
        return this.ram[a];
      },
      write: (a, v) => {
        if (a === L.sw_cmd) this.cmd = v;
        else this.ram[a] = v;
      },
    });
    this.ram[L.ascii] = 1;
  }

  until(stop) {
    this.cpu.run((cpu) => {
      if (cpu.pc === CHROUT) this.printed.push(cpu.a);
      return cpu.pc === stop;
    });
  }

  // The SwiftLink hands over one byte, with carrier up or down.
  receive(byte, carrier) {
    this.data = byte;
    this.status = (this.status & ~ST_DCD) | ST_RXFULL | (carrier ? 0 : ST_DCD);
    this.ram[0xfffa] = this.L.nmi & 0xff;
    this.ram[0xfffb] = this.L.nmi >> 8;
    this.cpu.pc = SENTINEL;
    this.cpu.s = 0xff;
    this.cpu.nmi();
    this.until(SENTINEL);
  }

  // The main loop's charin and show, for everything waiting.
  drain() {
    this.ram.set([0x20, this.L.charin & 0xff, this.L.charin >> 8, 0xb0, 0x03,
      0x20, this.L.show & 0xff, this.L.show >> 8, 0xea], SENTINEL);   // jsr charin; bcs +3; jsr show; nop
    while (this.ram[this.L.rhead] !== this.ram[this.L.rtail]) {
      this.cpu.pc = SENTINEL;
      this.cpu.s = 0xff;
      this.until(SENTINEL + 8);
    }
  }
}

const skip = !commandExists('acme') && !inCi && 'acme is not on PATH';

test('ASCII letters show in the other PETSCII case, so both cases read right in the lower-case set', { skip }, () => {
  const t = new Term(assemble());
  for (const c of 'aZ') t.receive(c.charCodeAt(0), true);
  t.drain();
  // PETSCII $41 is a lower-case "a"; $C1 and $61 both show as "A", so an "a" must arrive as $41
  assert.equal(t.printed[0], 0x41);
  assert.ok([0x7a, 0xda].includes(t.printed[1]), `"Z" printed as $${t.printed[1].toString(16)}`);
});

test('bytes received with carrier up are translated even if carrier drops before they are shown', { skip }, () => {
  const t = new Term(assemble());
  t.receive(0x61, true);   // the page's last "a"
  t.receive(0x93, true);   // an en dash's last UTF-8 byte; raw, it is PETSCII's clear screen
  t.receive(0x4e, false);  // NO CARRIER's "N"
  t.drain();
  assert.deepEqual(t.printed, [0x41, 0x4e]);
});
