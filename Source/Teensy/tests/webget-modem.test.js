// SPDX-License-Identifier: MIT
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const HARNESS_SOURCE = path.join(import.meta.dirname, 'webget_modem_harness.cpp');
const TEENSYROM_HANDLER = path.join(import.meta.dirname, '../MinimalBoot/Common/IO_Handlers/IOH_TeensyROM.c');

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('c++') && !inCi && 'c++ is not on PATH';

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'webget-modem-'));
  harnessPath = path.join(dir, 'harness');
  const built = spawnSync('c++', ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE],
    { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function run(lines) {
  const ran = spawnSync(harness(), { input: lines.join('\n') + '\n', encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.split(/\r?\n/).filter(Boolean).map(Number);
}

// The main image's table, as the source spells it: one "/* n ... */ value," entry a line.
function mainImageTable() {
  const source = fs.readFileSync(TEENSYROM_HANDLER, 'utf8');
  const body = source.match(/uint8_t ASCIItoPETSCII\[128\]\s*=\s*\{([\s\S]*?)\n\};/);
  assert.ok(body, 'ASCIItoPETSCII not found in IOH_TeensyROM.c');
  const values = [...body[1].matchAll(/\*\/\s*(\d+)/g)].map((match) => Number(match[1]));
  assert.equal(values.length, 128);
  return values;
}

test('the modem converts to PETSCII as the main image does, so its messages read the same', { skip }, () => {
  assert.deepEqual(run(['petscii']), mainImageTable());
});

test('a typed key keeps its case where the SwiftLink lower-cases it, and only there', { skip }, () => {
  const kept = (key) => run([`case ${key.toString(16)}`])[0];
  const stock = (key) => { let c = key & 0x7f; if (c & 0x40) c |= 0x20; return c; };   // SwiftTxBufToLcaseASSCII
  assert.equal(kept(0x41), 'a'.charCodeAt(0));   // unshifted A: lower case on the C64's text screen
  assert.equal(kept(0x5a), 'z'.charCodeAt(0));
  assert.equal(kept(0xc1), 'A'.charCodeAt(0));   // shifted
  assert.equal(kept(0xda), 'Z'.charCodeAt(0));
  assert.equal(kept(0x5f), '_'.charCodeAt(0));   // left arrow
  for (let key = 0; key < 256; key++) {
    if ((key >= 0x41 && key <= 0x5a) || (key >= 0xc1 && key <= 0xda) || key === 0x5f) continue;
    assert.equal(kept(key), stock(key), `key $${key.toString(16)}`);
  }
});

test('a .tls file is empty or a program DMA can place between the screen and I/O', { skip }, () => {
  const size = (n) => run([`size ${n}`])[0];
  const place = (load, len) => run([`place ${load.toString(16)} ${len.toString(16)}`])[0];
  assert.equal(size(0), 1);
  assert.equal(size(1), 0);
  assert.equal(size(2), 0);   // a load address and nothing to load
  assert.equal(size(3), 1);
  assert.equal(size(0xd000 - 0x0400 + 2), 1);
  assert.equal(size(0xd000 - 0x0400 + 3), 0);
  assert.equal(place(0x0801, 1), 1);
  assert.equal(place(0x0400, 0xcc00), 1);
  assert.equal(place(0x03ff, 1), 0);
  assert.equal(place(0x0801, 0xd000 - 0x0801), 1);
  assert.equal(place(0x0801, 0xd000 - 0x0800), 0);   // its last byte would be I/O
  assert.equal(place(0xcfff, 1), 1);
  assert.equal(place(0xc000, 0x1001), 0);
});
