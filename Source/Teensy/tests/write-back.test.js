// SPDX-License-Identifier: MIT
// When a disk image's writes go to the card (Source/Teensy/DiskImage/WriteBack.h), shared by the TR block device and VDrive.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const FIRMWARE_ROOT = path.resolve(import.meta.dirname, '..');
const HARNESS_SOURCE = path.join(FIRMWARE_ROOT, 'tests/write_back_harness.c');

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const missingTool = ['cc', 'c++'].find((tool) => !commandExists(tool));
const skip = missingTool && !inCi && `${missingTool} is not on PATH`;

const IDLE = 250;

let harnessPath;
function run(steps) {
  if (!harnessPath) {
    assert.equal(missingTool, undefined, 'CI installs every tool this test needs, so it must not skip there');
    harnessPath = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'write-back-')), 'harness');
    const built = spawnSync('cc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
    assert.equal(built.status, 0, built.stderr);
    const asCpp = spawnSync('c++', ['-x', 'c++', '-std=gnu++17', '-Wall', '-Werror', '-fsyntax-only', HARNESS_SOURCE], { encoding: 'utf8' });
    assert.equal(asCpp.status, 0, `the firmware compiles this as C++: ${asCpp.stderr}`);
  }
  const ran = spawnSync(harnessPath, [], { input: steps.join('\n') + '\n', encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.trim().split('\n').filter(Boolean);
}

test('nothing is due until a write, and then only once the writes have been idle a quarter second', { skip }, () => {
  assert.deepEqual(run(['due 1000', 'wrote 1000', `due ${1000 + IDLE}`, 'wrote 1200', `due ${1000 + IDLE + 1}`, `due ${1200 + IDLE + 1}`]),
                   ['0', '0', '0', '1']);
});

test('a sync that reaches the card clears the image', { skip }, () => {
  assert.deepEqual(run(['wrote 0', 'sync 1 300', 'dirty', 'due 10000']), ['0', '0', '0']);
});

test('a failed sync keeps the image dirty, retries after another idle period, and is reported once', { skip }, () => {
  assert.deepEqual(run([
    'wrote 0', 'sync 0 300', 'dirty', `due ${300 + IDLE}`, `due ${300 + IDLE + 1}`,
    'sync 0 600', 'sync 0 900', 'sync 1 1200', 'dirty', 'wrote 1300', 'sync 0 1600',
  ]), ['1', '1', '0', '1', '0', '0', '0', '0', '1']);
});

test('closing the image forgets what was pending', { skip }, () => {
  assert.deepEqual(run(['wrote 0', 'sync 0 300', 'forget', 'dirty', 'due 10000', 'wrote 20000', 'sync 0 20300']), ['1', '0', '0', '1']);
});
