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

// The file's two load-address bytes are not loaded: ROOM + 2 ends at $CFFF, one more byte would need $D000.
test('VERIFY takes a file ending at $CFFF and hands back one a byte longer', { skip }, () => {
  assert.match(run('verify', String(ROOM + 2)), / stat=5a$/);
  assert.match(run('verify', String(ROOM + 3)), new RegExp(` stat=${NOTMINE}$`));
});

// One mailbox command stalls the whole main loop (ASID, TCP, remote commands) for as long as it reads, and a card file has no
// size limit: the length pass stops once the file is known not to fit.
test('VERIFY of a file too long for RAM is handed back without reading the whole file', { skip }, () => {
  const [read, stat] = run('verify', String(4_000_000)).split(' ');
  assert.equal(stat, `stat=${NOTMINE}`);
  assert.ok(Number(read.slice('read='.length)) <= ROOM + DMA_PART, read);
});

// The device number a fast SAVE / LOAD was opened on can move before it ends (a U0> on the command channel, the bus scan). The
// transfer stays on its unit: before, the close looked the unit up by the old number, read v->unit[-1], left channel 1 open
// and the file unfinished, and a LOAD read zeros with no end.
test('a fast SAVE closes cleanly when its device is renumbered with U0> before the close', { skip }, () => {
  assert.equal(run('save', 'renumber'), 'close=5a data=0 open=0 blocks=1');
});

test('a fast SAVE closes cleanly when the bus scan releases its device number before the close', { skip }, () => {
  assert.equal(run('save', 'scan'), 'close=5a data=0 open=0 blocks=1');
});

test('a fast LOAD ends with its file when its device is renumbered after the open', { skip }, () => {
  assert.equal(run('load'), 'bytes=598 end=1');   // 600 bytes, the first two came with the open as the load address
});
