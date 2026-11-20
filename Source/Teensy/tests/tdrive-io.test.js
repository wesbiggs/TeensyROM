// SPDX-License-Identifier: MIT
// The TDrive mailbox's fast LOAD / SAVE / VERIFY paths (tdrive_io.c), as the TDrive KERNAL drives them.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const HARNESS_SOURCE = path.join(import.meta.dirname, 'tdrive_io_harness.c');

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('cc') && !inCi && 'cc is not on PATH';

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tdrive-io-'));
  harnessPath = path.join(dir, 'harness');
  const built = spawnSync('cc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function run(...args) {
  const ran = spawnSync(harness(), args, { encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.trim();
}

const NOTMINE = 'a5';
const ROOM = 0xd000 - 0x0801;   // a file loading at $0801 can be compared up to $CFFF
const DMA_PART = 2048;

test('VERIFY of a file that fits is taken by the fast path', { skip }, () => {
  assert.match(run('verify', '30000'), / stat=5a$/);
});

// One mailbox command stalls the whole main loop (ASID, TCP, remote commands) for as long as it reads, and a card file has no
// size limit: the length pass stops once the file is known not to fit.
test('VERIFY of a file too long for RAM is handed back without reading the whole file', { skip }, () => {
  const [read, stat] = run('verify', String(4_000_000)).split(' ');
  assert.equal(stat, `stat=${NOTMINE}`);
  assert.ok(Number(read.slice('read='.length)) <= ROOM + DMA_PART, read);
});
