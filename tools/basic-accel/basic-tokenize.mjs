// SPDX-License-Identifier: MIT
// BASIC V2 listing text to a .prg, as the C64 crunches a line typed at the prompt, for the
// listings in this folder. Lowercase is PETSCII's unshifted letters and uppercase its shifted
// ones, as `petcat -w2` reads a file; no {control} codes.

// $80 onwards, in the ROM's order, which is the order the cruncher tries them in
export const KEYWORDS = [
  'END', 'FOR', 'NEXT', 'DATA', 'INPUT#', 'INPUT', 'DIM', 'READ', 'LET', 'GOTO', 'RUN', 'IF', 'RESTORE', 'GOSUB',
  'RETURN', 'REM', 'STOP', 'ON', 'WAIT', 'LOAD', 'SAVE', 'VERIFY', 'DEF', 'POKE', 'PRINT#', 'PRINT', 'CONT', 'LIST',
  'CLR', 'CMD', 'SYS', 'OPEN', 'CLOSE', 'GET', 'NEW', 'TAB(', 'TO', 'FN', 'SPC(', 'THEN', 'NOT', 'STEP', '+', '-',
  '*', '/', '^', 'AND', 'OR', '>', '=', '<', 'SGN', 'INT', 'ABS', 'USR', 'FRE', 'POS', 'SQR', 'RND', 'LOG', 'EXP',
  'COS', 'SIN', 'TAN', 'ATN', 'PEEK', 'LEN', 'STR$', 'VAL', 'ASC', 'CHR$', 'LEFT$', 'RIGHT$', 'MID$', 'GO',
];
const DATA = 0x83;
const REM = 0x8f;

function petscii(ch) {
  const c = ch.charCodeAt(0);
  if (c >= 0x61 && c <= 0x7a) return c - 0x20;
  if (c >= 0x41 && c <= 0x5a) return c + 0x80;
  if (c >= 0x20 && c < 0x60) return c;
  throw new Error(`no PETSCII for ${JSON.stringify(ch)}`);
}

// One line's text after its number, crunched as the ROM's CRUNCH ($A579) does
export function crunch(text) {
  const bytes = [...text].map(petscii);
  const out = [];
  let quoted = false;
  let raw = null;  // DATA: until a colon outside quotes; REM: to the end
  for (let i = 0; i < bytes.length;) {
    const b = bytes[i];
    if (b === 0x22) quoted = !quoted;
    if (quoted || raw === REM || (raw === DATA && b !== 0x3a) || b === 0x22) {
      out.push(b);
      i++;
      continue;
    }
    raw = null;
    if (b === 0x3f) {  // ? is PRINT
      out.push(0x99);
      i++;
      continue;
    }
    // digits, colon and semicolon are copied without a keyword search
    const k = b >= 0x30 && b <= 0x3b ? -1 : KEYWORDS.findIndex((word) => [...word].every((ch, j) => bytes[i + j] === ch.charCodeAt(0)));
    if (k < 0) {
      out.push(b);
      i++;
      continue;
    }
    out.push(0x80 + k);
    i += KEYWORDS[k].length;
    if (0x80 + k === DATA || 0x80 + k === REM) raw = 0x80 + k;
  }
  return out;
}

export function tokenize(listing, load = 0x0801) {
  const lines = listing.split(/\r?\n/).filter((l) => l.trim() !== '');
  const body = [];
  let at = load;
  for (const line of lines) {
    const m = /^\s*(\d+)\s?(.*)$/.exec(line);
    if (!m) throw new Error(`no line number: ${line}`);
    const number = Number(m[1]);
    if (number > 63999) throw new Error(`line number ${number} is over 63999`);
    const tokens = crunch(m[2]);
    const next = at + 4 + tokens.length + 1;
    body.push(next & 0xff, next >> 8, number & 0xff, number >> 8, ...tokens, 0);
    at = next;
  }
  body.push(0, 0);
  return Uint8Array.from([load & 0xff, load >> 8, ...body]);
}

//   node tools/basic-accel/basic-tokenize.mjs LISTING.bas OUT.prg
if (process.argv[1] === import.meta.filename) {
  const [listing, out] = process.argv.slice(2);
  if (!listing || !out) {
    console.error('Usage: node tools/basic-accel/basic-tokenize.mjs LISTING.bas OUT.prg');
    process.exit(2);
  }
  const fs = await import('node:fs');
  fs.writeFileSync(out, tokenize(fs.readFileSync(listing, 'utf8')));
}
