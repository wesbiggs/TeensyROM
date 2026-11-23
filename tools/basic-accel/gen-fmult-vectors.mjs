// SPDX-License-Identifier: MIT
// Writes Source/Teensy/tests/cbm-fmult-vectors.txt: the C64 BASIC ROM's FMULTT results on the
// cases makeCases() generates, so cbm-fmult.test.js can check CbmFMult.h without the ROM.
//
//   node tools/basic-accel/gen-fmult-vectors.mjs     (needs the ROM; see findBasicRom)
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { findBasicRom, makeCases, runRom, resultLine } from './fmult-cases.mjs';

const LISTED = 256;
const HASHED = 50_000;
const SEED = 0x6502;

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const out = path.join(root, 'Source/Teensy/tests/cbm-fmult-vectors.txt');

const rom = findBasicRom();
if (!rom) {
  console.error('No C64 BASIC ROM (901226-01) found: install VICE with its ROMs, or set C64_BASIC_ROM.');
  process.exit(1);
}
const results = makeCases(HASHED, SEED).map((c) => resultLine(runRom(rom, c)));
const hash = createHash('sha256').update(results.join('\n')).digest('hex');
fs.writeFileSync(out, [
  '# FMULTT ($BA2B) as the C64 BASIC ROM 901226-01 computes it, on makeCases(n, 0x6502) from',
  '# tools/basic-accel/fmult-cases.mjs. Written by tools/basic-accel/gen-fmult-vectors.mjs.',
  '# Each line: "native", or "done", A X Y P, then $26-$29 $61-$66 $70. The hash covers all cases, one per line.',
  `sha256 ${HASHED} ${hash}`,
  ...results.slice(0, LISTED),
  '',
].join('\n'));
console.log(`wrote ${path.relative(root, out)}: ${LISTED} listed, ${HASHED} hashed`);
