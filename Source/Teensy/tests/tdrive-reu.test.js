// SPDX-License-Identifier: MIT
// The REU under "TDrive+REU" / "TDrive+Swift+REU": its $FF00 trigger shares the bus snoop with the replacement KERNAL's check.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const HARNESS_SOURCE = path.join(import.meta.dirname, 'tdrive_reu_harness.cpp');

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('c++') && !inCi && 'c++ is not on PATH';

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tdrive-reu-'));
  harnessPath = path.join(dir, 'harness');
  // the REU's DMA loops spin on an empty body, which the firmware's own build allows
  const built = spawnSync('c++', ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-Wno-empty-body', '-o', harnessPath,
    HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function run(lines) {
  const ran = spawnSync(harness(), { input: lines.join('\n') + '\n', encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.split(/\r?\n/).filter(Boolean);
}

const COMMAND = 0x01;
const command = (value) => [`d ${value.toString(16)}`, `w ${COMMAND.toString(16)}`];
const ARMED = 0x80;      // execute, on the next store to $FF00
const IMMEDIATE = 0x90;  // execute now ($FF00 decode off)

test('a command on the $FF00 trigger keeps the KERNAL served and starts on the store', { skip }, () => {
  assert.deepEqual(run(['kernal 0', ...command(ARMED), 'snoop 0', 'ff00 0', 'snoop 0']), ['armed', 'transfer', 'kernal']);
});

// As on the plain REU (IOH_REU.c): an immediate command replaces an armed one, so a later store to $FF00 (RAM under the
// KERNAL) starts nothing. Left armed, it ran a second transfer with whatever the registers held.
test('an immediate command disarms the $FF00 trigger', { skip }, () => {
  assert.deepEqual(run(['kernal 0', ...command(ARMED), ...command(IMMEDIATE), 'snoop 0', 'ff00 0', 'snoop 0']),
    ['kernal', 'none', 'kernal']);
});

// A program may set addresses or length after the command, or poll the status, before it stores to $FF00.
test('register writes and reads after the command keep the $FF00 trigger armed', { skip }, () => {
  assert.deepEqual(run(['kernal 0', ...command(ARMED), 'd 10', 'w 7', 'r 0', 'snoop 0', 'ff00 0', 'snoop 0']),
    ['armed', 'transfer', 'kernal']);
});

test('a command on the $FF00 trigger written again stays armed', { skip }, () => {
  assert.deepEqual(run(['kernal 0', ...command(ARMED), ...command(ARMED), 'snoop 0', 'ff00 0', 'snoop 0']),
    ['armed', 'transfer', 'kernal']);
});

test('an immediate command with nothing armed leaves the KERNAL check alone', { skip }, () => {
  assert.deepEqual(run(['kernal 0', ...command(IMMEDIATE), 'snoop 0']), ['kernal']);
});
