// SPDX-License-Identifier: MIT
// PETSKEY Remote's USB keyboard (UsbKbd.c): the C64's key bytes in, HID reports out, held to
// what netkbd_host.py (github.com/wesbiggs/petskey-remote) sends through uinput for the same keys.
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

function run(lines) {
  const ran = spawnSync(harness(), { input: lines.join('\n') + '\n', encoding: 'utf8' });
  assert.equal(ran.status, 0, ran.stderr);
  return ran.stdout.split(/\r?\n/).filter(Boolean);
}

// C64 key codes (column * 8 + row; 64+ the C128's) and Linux key codes, as PETSKEY sends them
const C64 = { RETURN: 1, CRSR_DOWN: 7, A: 10, LSHIFT: 15, C: 20, SEVEN: 24, PLUS: 40, MINUS: 43,
  RSHIFT: 52, CTRL: 58, TWO: 59, Q: 62, RUN_STOP: 63, ESC: 72, CAPS_LOCK: 89 };
const LINUX = { ESC: 1, TWO: 3, MINUS: 12, EQUAL: 13, Q: 16, ENTER: 28, LEFTCTRL: 29, A: 30, APOSTROPHE: 40,
  LEFTSHIFT: 42, C: 46, RIGHTSHIFT: 54, CAPSLOCK: 58, UP: 103, DOWN: 108, LINEFEED: 101, B: 48, D: 32,
  E: 18, F: 33, G: 34, H: 35 };

const DEFINED = 0x80, SETS_SHIFT = 1, SHIFT_ON = 2;
// bindings: [c64 code, 'plain'|'shift', linux key, shift: undefined (leave it) | true | false]
function keymap(bindings) {
  const map = new Uint8Array(90 * 4);
  for (const [code, table, key, shift] of bindings) {
    const at = code * 4 + (table === 'shift' ? 2 : 0);
    map[at] = DEFINED | (shift === undefined ? 0 : SETS_SHIFT | (shift ? SHIFT_ON : 0));
    map[at + 1] = key;
  }
  return [...map];
}

// The symbolic C64 keymap's lines these tests use
const SYMBOLIC = keymap([
  [C64.A, 'plain', LINUX.A], [C64.C, 'plain', LINUX.C], [C64.Q, 'plain', LINUX.Q],
  [C64.RETURN, 'plain', LINUX.ENTER], [C64.CTRL, 'plain', LINUX.LEFTCTRL],
  [C64.LSHIFT, 'plain', LINUX.LEFTSHIFT], [C64.RSHIFT, 'plain', LINUX.RIGHTSHIFT],
  [C64.TWO, 'plain', LINUX.TWO], [C64.TWO, 'shift', LINUX.APOSTROPHE, true],
  [C64.SEVEN, 'shift', LINUX.APOSTROPHE, false],
  [C64.PLUS, 'plain', LINUX.EQUAL, true],
  [C64.CRSR_DOWN, 'plain', LINUX.DOWN], [C64.CRSR_DOWN, 'shift', LINUX.UP, false],
  [C64.RUN_STOP, 'plain', LINUX.ESC], [C64.ESC, 'plain', LINUX.ESC],
  [C64.CAPS_LOCK, 'plain', LINUX.CAPSLOCK],
  [C64.MINUS, 'plain', LINUX.LINEFEED],
]);

const hex = (bytes) => 'feed ' + bytes.map((b) => b.toString(16).padStart(2, '0')).join(' ');
const hello = (version = 2, flags = 0) => hex([0xf0, version, flags]);
const sendMap = (map) => hex([0xf1, map.length & 0xff, map.length >> 8, ...map]);
const down = (code) => hex([code]);
const up = (code) => hex([code | 0x80]);
const live = (map = SYMBOLIC) => ['start', hello(), sendMap(map)];
const keys = (...args) => args.map(([dir, code]) => (dir === '+' ? down : up)(code));

// The HID side, as the host sees it: modifier byte, then the six key slots
const report = (mods, ...usages) => `report ${[mods, ...usages, 0, 0, 0, 0, 0, 0].slice(0, 7)
  .map((b) => b.toString(16).padStart(2, '0')).join(' ')}`;
const NONE = report(0);
const LSHIFT = 0x02, LCTRL = 0x01;
const HID = { A: 0x04, C: 0x06, Q: 0x14, ESC: 0x29, ENTER: 0x28, TWO: 0x1f, APOSTROPHE: 0x34, DOWN: 0x51,
  UP: 0x52, CAPSLOCK: 0x39, EQUAL: 0x2e, B: 0x05, D: 0x07, E: 0x08, F: 0x09, G: 0x0a, H: 0x0b };

const typed = (...lines) => run([...live(), ...lines]).filter((l) => l.startsWith('report'));

test('the hello is greeted, and the modem echo before it is ignored', { skip }, () => {
  const echo = hex([...Buffer.from('atdt usb\r')]);
  assert.deepEqual(run(['start', echo, hello()]), ['connected', 'rx Linked to USB keyboard.']);
  assert.deepEqual(run(['host 0', 'start', hello()]), ['connected', 'rx Linked to USB keyboard (no USB host).']);
});

test('a protocol 1 C64 is told to update, and its keys type nothing', { skip }, () => {
  assert.deepEqual(run(['start', hello(1), down(C64.A), up(C64.A)]),
    ['connected', 'rx This PETSKEY sends no keymap: update it.']);
});

test('keys before the keymap arrives type nothing', { skip }, () => {
  assert.deepEqual(run(['start', hello(), down(C64.A), up(C64.A), sendMap(SYMBOLIC), down(C64.A)]),
    ['connected', 'rx Linked to USB keyboard.', report(0, HID.A)]);
});

test('a plain letter, and shift passing through', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.A], ['-', C64.A], ['+', C64.LSHIFT], ['+', C64.A], ['-', C64.A], ['-', C64.LSHIFT])),
    [report(0, HID.A), NONE, report(LSHIFT), report(LSHIFT, HID.A), report(LSHIFT), NONE]);
});

test('either C64 SHIFT is the one left shift', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.RSHIFT], ['+', C64.LSHIFT], ['-', C64.RSHIFT], ['-', C64.LSHIFT])),
    [report(LSHIFT), NONE]);
});

test('SHIFT+2 is a double quote: shift held, apostrophe', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.RSHIFT], ['+', C64.TWO], ['-', C64.TWO], ['-', C64.RSHIFT])),
    [report(LSHIFT), report(LSHIFT, HID.APOSTROPHE), report(LSHIFT), NONE]);
});

test('SHIFT+7 is an apostrophe: shift lifted while it is down, then back', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.LSHIFT], ['+', C64.SEVEN], ['-', C64.SEVEN], ['+', C64.A], ['-', C64.A], ['-', C64.LSHIFT])),
    [report(LSHIFT), NONE, report(0, HID.APOSTROPHE), NONE, report(LSHIFT), report(LSHIFT, HID.A), report(LSHIFT), NONE]);
});

test('a key whose binding needs shift adds it', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.PLUS], ['-', C64.PLUS])),
    [report(LSHIFT), report(LSHIFT, HID.EQUAL), report(LSHIFT), NONE]);
});

test('a binding is fixed when its key goes down: SHIFT released before CRSR still releases Up', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.LSHIFT], ['+', C64.CRSR_DOWN], ['-', C64.LSHIFT], ['-', C64.CRSR_DOWN])),
    [report(LSHIFT), NONE, report(0, HID.UP), NONE]);
});

test('modifiers combine with keys', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.CTRL], ['+', C64.C], ['-', C64.C], ['-', C64.CTRL])),
    [report(LCTRL), report(LCTRL, HID.C), report(LCTRL), NONE]);
});

test('CAPS LOCK taps once each time it locks or unlocks', { skip }, () => {
  assert.deepEqual(typed(down(C64.CAPS_LOCK), up(C64.CAPS_LOCK)),
    [report(0, HID.CAPSLOCK), NONE, report(0, HID.CAPSLOCK), NONE]);
});

test('two C64 keys on one target press it once and release it with the last', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.RUN_STOP], ['+', C64.ESC], ['-', C64.RUN_STOP], ['-', C64.ESC])),
    [report(0, HID.ESC), NONE]);
});

test('a stray key up, an unmapped key, a key with no HID usage, and codes past 89 type nothing', { skip }, () => {
  assert.deepEqual(typed(up(C64.A), down(C64.SEVEN), up(C64.SEVEN), down(C64.MINUS), up(C64.MINUS), hex([0x5a, 0xda])), []);
});

test('a seventh key is dropped, and releases close the gaps', { skip }, () => {
  const map = keymap(['A', 'B', 'C', 'D', 'E', 'F', 'G'].map((k, i) => [i, 'plain', LINUX[k] ?? LINUX.A]));
  const out = run(['start', hello(), sendMap(map), ...[0, 1, 2, 3, 4, 5, 6].map(down), up(1), down(6)])
    .filter((l) => l.startsWith('report'));
  assert.deepEqual(out.slice(5), [
    report(0, HID.A, HID.B, HID.C, HID.D, HID.E, HID.F),
    report(0, HID.A, HID.C, HID.D, HID.E, HID.F)]);
  // G stays unsent: like a 6KRO keyboard, a key that found no slot is not resent later
  assert.equal(out.length, 7);
});

test('three seconds without a byte release everything, once', { skip }, () => {
  assert.deepEqual(typed(...keys(['+', C64.LSHIFT], ['+', C64.Q]), 'ms 2999', 'poll', 'ms 1', 'poll', 'ms 5000', 'poll'),
    [report(LSHIFT), report(LSHIFT, HID.Q), NONE]);
  // a heartbeat holds it off
  assert.deepEqual(typed(down(C64.Q), 'ms 2000', hex([0xfe]), 'ms 2000', 'poll'), [report(0, HID.Q)]);
});

test('bye releases everything; a new hello needs its keymap again', { skip }, () => {
  assert.deepEqual(run([...live(), down(C64.Q), hex([0xfc]), hex([...Buffer.from('+++')]), down(C64.A), hello(), down(C64.A)]),
    ['connected', 'rx Linked to USB keyboard.', report(0, HID.Q), NONE, 'rx Linked to USB keyboard.']);
});

test('a keymap longer than this firmware knows is read to its end', { skip }, () => {
  assert.deepEqual(typed(sendMap([...SYMBOLIC, 0x80, 0x1e, 0xf0, 0xf1]), down(C64.A)), [report(0, HID.A)]);
});

test('hanging up releases what is held, and the next dial starts clean', { skip }, () => {
  assert.deepEqual(run([...live(), down(C64.A), 'end', down(C64.A), 'start', down(C64.A)]),
    ['connected', 'rx Linked to USB keyboard.', report(0, HID.A), NONE, 'connected']);
});

test('ATDT USB, quoted or with a port, is the USB keyboard; anything else is a network host', { skip }, () => {
  const names = ['usb', 'usb:0', '"usb"', 'usb:6526', 'usbhost', 'us', '192.168.7.1:6526', 'usb.example.com'];
  assert.deepEqual(run(names.map((n) => `isusb ${n}`)), ['yes', 'yes', 'yes', 'yes', 'no', 'no', 'no', 'no']);
});
