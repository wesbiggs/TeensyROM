// SPDX-License-Identifier: MIT
// TDrive's remote image selection: the host app's set, and the sidecar the next start mounts from.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const HARNESS_SOURCE = path.join(import.meta.dirname, 'tdrive_mount_harness.cpp');

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('c++') && !inCi && 'c++ is not on PATH';

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tdrive-mount-'));
  harnessPath = path.join(dir, 'harness');
  // the handler's callbacks take a context they do not all use, as the firmware's own build allows
  const built = spawnSync('c++', ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function',
    '-Wno-format-truncation', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

function run(lines) {
  const ran = spawnSync(harness(), { input: lines.join('\n') + '\n', encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.split(/\r?\n/).filter(Boolean);
}

const D64_SIZE = 174848;

test('a remote set mounts a disk image', { skip }, () => {
  assert.deepEqual(run([`file /GAME.D64 ${D64_SIZE}`, 'set /GAME.D64']), ['0 /GAME.D64']);
});

// The format comes from the size alone, so a 1 MB cartridge mounted as a writable D81 and a SAVE wrote sectors into it.
test('a remote set refuses a file that is not a disk image, and the mounted disk stays', { skip }, () => {
  assert.deepEqual(run([`file /GAME.D64 ${D64_SIZE}`, 'file /BIG.CRT 1048576', 'set /GAME.D64', 'set /BIG.CRT']),
    ['0 /GAME.D64', '74 /GAME.D64 : not a disk image']);
});

test('with the menu showing, a set never remembers a file that is not a disk image', { skip }, () => {
  assert.deepEqual(run(['file /BIG.CRT 1048576', 'idle /BIG.CRT']), ['74 none : not a disk image']);
});
