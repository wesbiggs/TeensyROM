// SPDX-License-Identifier: MIT
// BasicEmu.h on made-up ROMs: the 6510's banking, I/O to the bus, the float hooks' dispatch and
// fallback, and what the VIC flush writes. tools/basic-accel/emu-model.test.mjs runs it on the
// C64's ROMs where they are installed.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const HARNESS_SOURCE = path.join(import.meta.dirname, 'basic_emu_harness.c');
const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('cc') && !inCi && 'cc is not on PATH';

test('BasicEmu.h banks, routes, hooks and flushes as the C64 would', { skip }, () => {
  const exe = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'basic-emu-')), 'basic_emu_harness');
  const built = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  const ran = spawnSync(exe, [], { encoding: 'utf8' });
  const lines = ran.stdout.trim().split('\n');
  assert.deepEqual(lines.filter((l) => !l.startsWith('ok ')), []);
  assert.equal(ran.status, 0, ran.stderr);
  assert.ok(lines.length >= 20, `only ${lines.length} checks ran`);
});
