// SPDX-License-Identifier: MIT
// The TDrive engine (Source/Teensy/MinimalBoot/Common/tdrive/tdrive.c), driven through its mailbox as the KERNAL does.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const FIRMWARE_ROOT = path.resolve(import.meta.dirname, '..');
const HARNESS_SOURCE = path.join(FIRMWARE_ROOT, 'tests/tdrive_harness.c');

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const missingTool = ['cc', 'c++'].find((tool) => !commandExists(tool));
const skip = missingTool && !inCi && `${missingTool} is not on PATH`;

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  assert.equal(missingTool, undefined, 'CI installs every tool this test needs, so it must not skip there');
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tdrive-'));
  harnessPath = path.join(dir, 'harness');
  const built = spawnSync('cc', ['-std=c99', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

const runHarness = (...args) => {
  const ran = spawnSync(harness(), args, { encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.trim();
};

test('LOAD reads back every byte of a program saved on the disk, with EOI on the last and "00, OK" after', { skip }, () => {
  assert.equal(runHarness('load'), 'bytes=600 same=1 eoi=1 status=00, OK,00,00');
});

test('LOAD of a name that is not on the disk delivers nothing and reports 62', { skip }, () => {
  assert.equal(runHarness('missing'), 'bytes=0 status=62,FILE NOT FOUND,00,00');
});

// OPEN 3,8,3,"SPEC": the drive prefix, the file type letter and the mode letter in the name decide what is created
const OPEN_SPECS = [
  ['0:NEW,S,W', 'NEW', 'status=00, OK,00,00 type=SEQ'],
  ['NEW,P,W', 'NEW', 'status=00, OK,00,00 type=PRG'],
  ['NEW,U,W', 'NEW', 'status=00, OK,00,00 type=USR'],
  ['NEW,W', 'NEW', 'status=00, OK,00,00 type=SEQ'],
  ['HELLO,S,W', 'HELLO', 'status=63,FILE EXISTS,00,00 type=PRG'],
  ['NOPE', 'NOPE', 'status=62,FILE NOT FOUND,00,00 type=none'],
];
for (const [spec, name, expected] of OPEN_SPECS) {
  test(`OPEN "${spec}" on a disk holding HELLO`, { skip }, () => {
    assert.equal(runHarness('open', spec, name), expected);
  });
}

const SCRATCHES = [
  ['S:HELLO', 'status=01,FILES SCRATCHED,01,00 files=1'],
  ['S0:HELLO*', 'status=01,FILES SCRATCHED,02,00 files=0'],
  ['S:HELLO,0:HELLO2', 'status=01,FILES SCRATCHED,02,00 files=0'],
  ['S:NOPE', 'status=01,FILES SCRATCHED,00,00 files=2'],
];
for (const [command, expected] of SCRATCHES) {
  test(`"${command}" on a disk holding HELLO and HELLO2`, { skip }, () => {
    assert.equal(runHarness('scratch', command), expected);
  });
}

test('a host name the C64 cannot use gets an alias of at most 16 characters that keeps its start and end', { skip }, () => {
  assert.equal(runHarness('alias', 'The-Pumpkins-Breadbin-of-Horror-side1.D64'), '1 THE-PU^SIDE1.D64');
  assert.equal(runHarness('alias', 'a*b.d64'), '1 A^B.D64');
  assert.equal(runHarness('alias', 'hello'), '0 HELLO');
  // a last word of more than 5 characters is cut to its last 3
  assert.equal(runHarness('alias', 'a-very-long-name-with-longerword.d64'), '1 A-VERY-L^ORD.D64');
});
