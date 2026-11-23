// SPDX-License-Identifier: MIT
// Writes Source/Teensy/tests/cbm-float-vectors.txt: the C64 BASIC ROM's results for each float
// routine on the cases makeCases() generates, so cbm-float.test.js can check CbmFloat.h without
// the ROM.
//
//   node tools/basic-accel/gen-float-vectors.mjs     (needs the ROM; see findBasicRom)
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { findBasicRom, casesFor, runRom, resultLine, ROUTINES } from './float-cases.mjs';

const LISTED = 64;
const HASHED = 20_000;
const SEED = 0x6502;

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const out = path.join(root, 'Source/Teensy/tests/cbm-float-vectors.txt');

const rom = findBasicRom();
if (!rom) {
  console.error('No C64 BASIC ROM (901226-01) found: install VICE with its ROMs, or set C64_BASIC_ROM.');
  process.exit(1);
}
const lines = [
  '# The C64 BASIC ROM 901226-01\'s float routines on casesFor(routine, n, 0x6502) from',
  '# tools/basic-accel/float-cases.mjs. Written by tools/basic-accel/gen-float-vectors.mjs.',
  '# Per routine: "sha256 ROUTINE COUNT HASH" over all its results, one per line, then the first',
  '# results listed as "ROUTINE native", or "ROUTINE done", A X Y P, and the zero page OUTPUT_ZP names.',
];
for (const routine of Object.keys(ROUTINES)) {
  const results = casesFor(routine, HASHED, SEED).map((c) => resultLine(runRom(rom, routine, c)));
  lines.push(`sha256 ${routine} ${HASHED} ${createHash('sha256').update(results.join('\n')).digest('hex')}`);
  lines.push(...results.slice(0, LISTED).map((r) => `${routine} ${r}`));
}
fs.writeFileSync(out, lines.join('\n') + '\n');
console.log(`wrote ${path.relative(root, out)}: ${LISTED} listed and ${HASHED} hashed per routine`);
