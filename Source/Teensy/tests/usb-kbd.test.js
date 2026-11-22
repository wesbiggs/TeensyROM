// SPDX-License-Identifier: MIT
// The USB Keyboard Special IO (IOH_UsbKbd.c): HID usage presses and releases written at
// $DE00, boot-protocol reports out of the device port.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

import { commandExists } from '../../../tools/lib/toolchain.mjs';

const HARNESS_SOURCE = path.join(import.meta.dirname, 'usb_kbd_harness.cpp');

const NOT_CI = ['', '0', 'false'];
const inCi = !NOT_CI.includes(process.env.CI ?? '');
const skip = !commandExists('c++') && !inCi && 'c++ is not on PATH';

let harnessPath;
function harness() {
  if (harnessPath) return harnessPath;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'usb-kbd-'));
  harnessPath = path.join(dir, 'harness');
  const built = spawnSync('c++', ['-std=gnu++17', '-Wall', '-Wextra', '-Werror', '-o', harnessPath, HARNESS_SOURCE], { encoding: 'utf8' });
  assert.equal(built.status, 0, built.stderr);
  return harnessPath;
}

// Each test starts the handler; its first, empty report is left out of what run() returns
function run(lines) {
  const ran = spawnSync(harness(), { input: ['init', ...lines].join('\n') + '\n', encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  const out = ran.stdout.split(/\r?\n/).filter(Boolean);
  assert.match(out[0], /^report 00 00 00 00 00 00 00/);
  return out.slice(1);
}

const press = (usage) => `w 0 ${usage.toString(16)}`;
const release = (usage) => `w 1 ${usage.toString(16)}`;
const report = (mods, ...keys) =>
  'report ' + [mods, ...keys, ...Array(6 - keys.length).fill(0)].map((b) => b.toString(16).padStart(2, '0')).join(' ');
const EMPTY = report(0);
const A = 0x04, B = 0x05, C = 0x06, D = 0x07, E = 0x08, F = 0x09, G = 0x0a, LSHIFT = 0xe1, LCTRL = 0xe0;

test('the ID, version and status read back; nothing past them answers', { skip }, () => {
  assert.deepEqual(run(['r 4', 'r 5', 'r 6', 'r 2', 'r 0', 'r 3', 'r 7', 'r ff']),
    ['read 54', 'read 52', 'read 4b', 'read 01', 'read 01', 'read 00', 'open', 'open']);
});

test('a press and a release are each a report, even within one poll', { skip }, () => {
  assert.deepEqual(run([press(A), 'poll', release(A), 'poll']), [report(0, A), EMPTY]);
  assert.deepEqual(run([press(A), release(A), 'poll']), [report(0, A), EMPTY]);
});

test('presses are counted per usage', { skip }, () => {
  assert.deepEqual(run([press(A), press(A), release(A), 'poll', release(A), 'poll', release(A), 'poll']),
    [report(0, A), EMPTY]);
});

test('modifiers go in the modifier byte', { skip }, () => {
  assert.deepEqual(run([press(LSHIFT), press(A), press(LCTRL), release(LSHIFT), 'poll']),
    [report(2), report(2, A), report(3, A), report(1, A)]);
});

test('keys stay in their slots, and a seventh waits for one', { skip }, () => {
  assert.deepEqual(run([A, B, C, D, E, F, G].map(press).concat([release(B), release(G), release(A), 'poll'])), [
    report(0, A), report(0, A, B), report(0, A, B, C), report(0, A, B, C, D), report(0, A, B, C, D, E),
    report(0, A, B, C, D, E, F), report(0, A, C, D, E, F, G), report(0, A, C, D, E, F), report(0, C, D, E, F),
  ]);
});

test('usages outside the boot report are ignored', { skip }, () => {
  assert.deepEqual(run([press(0), press(1), press(3), press(0x80), press(0xe8), press(0xff), release(A), 'poll']), []);
});

test('writing $DE02 releases everything, and so does the handler going away', { skip }, () => {
  assert.deepEqual(run([press(A), press(LSHIFT), 'w 2 0', 'poll', release(A), 'poll']),
    [report(0, A), report(2, A), EMPTY]);
  assert.deepEqual(run([press(A), 'poll', 'end']), [report(0, A), EMPTY]);
  assert.deepEqual(run(['end']), []);
});

test('a full queue says so, drops writes, and still takes a release-all', { skip }, () => {
  const fill = Array.from({ length: 63 }, (_, i) => press(0x04 + (i % 6)));
  const out = run([...fill, 'r 0', press(G), 'poll', 'r 0', release(A), 'poll']);
  assert.equal(out[0], 'read 03');
  assert.ok(!out.some((l) => l.includes(' 0a')), 'the 64th write was dropped');
  assert.equal(out.at(-2), report(0, A, B, C, D, E, F));
  assert.equal(out.at(-1), 'read 01', 'drained, and A is still held: it was pressed 11 times');

  const allUp = run([...fill, 'w 2 0', 'poll', press(B), 'poll']);
  assert.deepEqual(allUp, [EMPTY, report(0, B)], 'the presses queued before it never land');
});

test('held keys are released after 3 s with no register written', { skip }, () => {
  assert.deepEqual(run([press(A), 'poll', 'ms 2999', 'poll', 'ms 1', 'poll', 'ms 5000', 'poll']),
    [report(0, A), EMPTY]);
  assert.deepEqual(run([press(A), 'poll', 'ms 2000', 'w 3 fe', 'ms 2000', 'poll', 'ms 1000', 'poll']),
    [report(0, A), EMPTY]);
});

test('with no USB host the report waits for one, and the status says so', { skip }, () => {
  const out = run(['host 0', 'poll', 'r 0', press(A), 'poll', 'poll', 'host 1', 'poll', 'poll', 'r 0']);
  assert.deepEqual(out, ['read 00', report(0, A) + ' (no host)', report(0, A), 'read 01']);
});

test('the host LEDs read back at $DE01', { skip }, () => {
  assert.deepEqual(run(['leds 2', 'poll', 'r 1']), ['read 02']);
});
