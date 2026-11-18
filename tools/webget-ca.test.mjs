// SPDX-License-Identifier: MIT
// The committed ca.pem and the root list in webget-ca.mjs must not drift apart: rebuilding
// the file from itself has to reproduce it byte for byte.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ROOTS, selectRoots, certificates } from './webget-ca.mjs';

const repoRoot = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const caPem = fs.readFileSync(path.join(repoRoot, 'Source', 'Teensy', 'WebGet', 'ca.pem'), 'utf8');

test('the committed ca.pem is exactly the named roots, in order', () => {
  const roots = selectRoots(caPem, new Date('2026-11-18'));
  assert.equal(roots.length, ROOTS.length);
  assert.equal(roots.map(({ pem }) => pem).join(''), caPem);
});

test('the committed ca.pem stays under the 64 KiB the host reads', () => {
  assert.ok(caPem.length < 64 * 1024);
});

test('a bundle missing a named root writes nothing', () => {
  const withoutFirst = certificates(caPem).slice(1).map(({ pem }) => pem).join('');
  assert.throws(() => selectRoots(withoutFirst, new Date('2026-11-18')), /ISRG Root X1/);
});

test('an expired root counts as missing', () => {
  assert.throws(() => selectRoots(caPem, new Date('2029-01-01')), /AAA Certificate Services/);
});
