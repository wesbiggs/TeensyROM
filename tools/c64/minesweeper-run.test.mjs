// SPDX-License-Identifier: MIT
// Runs the menu's Minesweeper from its committed header (build-c64.test.mjs keeps that header in step
// with the source) and checks the game against a reference written here.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { Mos6502 } from '../lib/mos6502.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

const header = fs.readFileSync(path.join(root, 'Source/Teensy/TRMenuFiles/ROMs/Minesweeper.prg.h'), 'utf8');
const prg = [...header.slice(header.indexOf('{')).matchAll(/0x([0-9a-f]{2})/gi)].map((m) => parseInt(m[1], 16));
const LOAD = prg[0] | (prg[1] << 8);

// The source's "name = $hex" lines: its entry points, variables and constants
const S = Object.fromEntries(
  [...fs.readFileSync(path.join(root, 'Source/C64/Minesweeper/source/Minesweeper.asm'), 'utf8').matchAll(/^(\w+)\s*=\s*\$([0-9a-f]+)/gim)]
    .map((m) => [m[1], parseInt(m[2], 16)]),
);
const STATE = { READY: 0, PLAYING: 1, WON: 2, LOST: 3 };
const LEVELS = [
  { rows: 9, cols: 9, mines: 10, left: 15 },
  { rows: 16, cols: 16, mines: 40, left: 12 },
  { rows: 16, cols: 30, mines: 99, left: 5 },
];
const TOP = 5;
const CALLER = 0x0200;
const id = (text) => [...text].map((c) => c.charCodeAt(0));

// The TR's page as USBMouse.c and the block device serve it
function trPage(mouse) {
  return (offset) => {
    if (offset === 0) return mouse.dx & 0xff;
    if (offset === 1) return mouse.dy & 0xff;
    if (offset === 2) return mouse.buttons;
    if (offset >= 3 && offset < 6) return id('TRM')[offset - 3];
    if (offset >= 0x10 && offset < 0x13) return id('TRB')[offset - 0x10];
    return 0;
  };
}

class Game {
  constructor({ level = 0, seed = 0x1234, pages = {} } = {}) {
    this.ram = new Uint8Array(0x10000);
    this.ram.set(prg.slice(2), LOAD);
    this.pages = pages;
    this.cpu = new Mos6502({
      read: (a) => (this.pages[a & 0xff00] ? this.pages[a & 0xff00](a & 0xff) : this.ram[a]),
      write: (a, v) => { if (!this.pages[a & 0xff00]) this.ram[a] = v; },
    });
    this.ram[S.level] = level;
    this.ram[S.seed] = seed & 0xff;
    this.ram[S.seed + 1] = seed >> 8;
    this.call(S.API_NEWGAME);
  }

  call(address) {
    this.ram.set([0x20, address & 0xff, address >> 8], CALLER);
    this.cpu.pc = CALLER;
    this.cpu.s = 0xff;
    this.cpu.run((c) => c.pc === CALLER + 3, 20_000_000);
  }

  at(entry, r, c) {
    this.ram[S.row] = r;
    this.ram[S.col] = c;
    this.call(entry);
  }

  get rows() { return this.ram[S.rows]; }
  get cols() { return this.ram[S.cols]; }
  get state() { return this.ram[S.state]; }
  get safeLeft() { return this.ram[S.safeLeft] | (this.ram[S.safeLeft + 1] << 8); }
  get flags() { return this.ram[S.flags] | (this.ram[S.flags + 1] << 8); }
  cell(r, c) { return this.ram[S.BOARD + r * 32 + c]; }
  isMine(r, c) { return (this.cell(r, c) & 0x80) !== 0; }
  isOpen(r, c) { return (this.cell(r, c) & 0x40) !== 0; }
  isFlag(r, c) { return (this.cell(r, c) & 0x20) !== 0; }
  count(r, c) { return this.cell(r, c) & 0x0f; }

  *cells() {
    for (let r = 0; r < this.rows; r++) for (let c = 0; c < this.cols; c++) yield [r, c];
  }

  *neighbors(r, c) {
    for (let dr = -1; dr <= 1; dr++) {
      for (let dc = -1; dc <= 1; dc++) {
        const nr = r + dr, nc = c + dc;
        if ((dr || dc) && nr >= 0 && nr < this.rows && nc >= 0 && nc < this.cols) yield [nr, nc];
      }
    }
  }

  // What opening r, c should open, by the rules, from the mines the game placed
  expectedOpen(r, c, open = new Set()) {
    const todo = [[r, c]];
    while (todo.length) {
      const [tr, tc] = todo.pop();
      const key = `${tr},${tc}`;
      if (open.has(key) || this.isFlag(tr, tc)) continue;
      open.add(key);
      if ([...this.neighbors(tr, tc)].every(([nr, nc]) => !this.isMine(nr, nc))) todo.push(...this.neighbors(tr, tc));
    }
    return open;
  }

  openSet() {
    return new Set([...this.cells()].filter(([r, c]) => this.isOpen(r, c)).map(([r, c]) => `${r},${c}`));
  }

  screen(row, col) { return this.ram[S.SCREEN + row * 40 + col]; }
}

for (const [level, spec] of LEVELS.entries()) {
  for (const seed of [0x0001, 0x1234, 0xbeef]) {
    test(`level ${level}, seed ${seed.toString(16)}: the first click places every mine away from it and opens what the rules open`, () => {
      const g = new Game({ level, seed });
      assert.equal(g.rows, spec.rows);
      assert.equal(g.cols, spec.cols);
      assert.equal(g.state, STATE.READY);
      const r = spec.rows >> 1, c = spec.cols >> 1;
      g.at(S.API_LEFT, r, c);
      assert.equal(g.state, STATE.PLAYING);

      const mines = [...g.cells()].filter(([mr, mc]) => g.isMine(mr, mc));
      assert.equal(mines.length, spec.mines);
      for (const [mr, mc] of mines) assert.ok(Math.abs(mr - r) > 1 || Math.abs(mc - c) > 1, `a mine at ${mr},${mc} beside the first click`);
      for (let x = spec.cols; x < 32; x++) for (let y = 0; y < 16; y++) assert.equal(g.ram[S.BOARD + y * 32 + x], 0, 'a cell past the board was written');
      for (const [cr, cc] of g.cells()) {
        assert.equal(g.count(cr, cc), [...g.neighbors(cr, cc)].filter(([nr, nc]) => g.isMine(nr, nc)).length, `count at ${cr},${cc}`);
      }
      assert.deepEqual(g.openSet(), g.expectedOpen(r, c));
      assert.equal(g.safeLeft, spec.rows * spec.cols - spec.mines - g.openSet().size);
    });
  }

  test(`level ${level}: opening every safe cell wins and flags every mine`, () => {
    const g = new Game({ level, seed: 0x4321 + level });
    g.at(S.API_LEFT, 0, 0);
    for (const [r, c] of g.cells()) {
      if (!g.isMine(r, c) && !g.isOpen(r, c)) g.at(S.API_LEFT, r, c);
    }
    assert.equal(g.state, STATE.WON);
    assert.equal(g.safeLeft, 0);
    assert.equal(g.flags, spec.mines);
    for (const [r, c] of g.cells()) assert.equal(g.isFlag(r, c), g.isMine(r, c));
  });
}

test('opening a mine loses, marks it, and stops play', () => {
  const g = new Game();
  g.at(S.API_LEFT, 4, 4);
  const [mr, mc] = [...g.cells()].find(([r, c]) => g.isMine(r, c));
  g.at(S.API_LEFT, mr, mc);
  assert.equal(g.state, STATE.LOST);
  assert.equal(g.cell(mr, mc) & 0x10, 0x10);
  const [sr, sc] = [...g.cells()].find(([r, c]) => !g.isMine(r, c) && !g.isOpen(r, c));
  g.at(S.API_LEFT, sr, sc);
  g.at(S.API_RIGHT, sr, sc);
  assert.equal(g.isOpen(sr, sc) || g.isFlag(sr, sc), false, 'a lost game still took a move');
});

test('a flag counts, blocks opening, and comes off again; an open cell takes no flag', () => {
  const g = new Game();
  g.at(S.API_RIGHT, 0, 0);
  assert.ok(g.isFlag(0, 0));
  assert.equal(g.flags, 1);
  g.at(S.API_LEFT, 0, 0);
  assert.equal(g.isOpen(0, 0), false);
  assert.equal(g.state, STATE.READY, 'clicking a flag placed the mines');
  g.at(S.API_RIGHT, 0, 0);
  assert.equal(g.flags, 0);
  g.at(S.API_LEFT, 4, 4);
  g.at(S.API_RIGHT, 4, 4);
  assert.equal(g.isFlag(4, 4), false);
  assert.equal(g.flags, 0);
});

test('more flags than mines show a negative count', () => {
  const g = new Game();
  for (let c = 0; c < 9; c++) for (const r of [0, 1]) g.at(S.API_RIGHT, r, c);
  assert.equal(g.flags, 18);
  // "-008" from column 1 of the status row
  assert.deepEqual([1, 2, 3, 4].map((col) => g.screen(2, col)), [0x2d, 0x30, 0x30, 0x38]);
});

// A number whose flags match opens its other neighbors; with a flag on the wrong cell it opens a mine.
function numberBesideCovered(g) {
  return [...g.cells()].find(([r, c]) => g.isOpen(r, c) && g.count(r, c) > 0
    && [...g.neighbors(r, c)].some(([nr, nc]) => !g.isOpen(nr, nc) && !g.isMine(nr, nc)));
}

test('opening around a number with its mines flagged opens the rest, by left or middle click', () => {
  for (const entry of [S.API_LEFT, S.API_MIDDLE]) {
    const g = new Game({ level: 1, seed: 0x7777 });
    g.at(S.API_LEFT, 8, 8);
    const [r, c] = numberBesideCovered(g);
    const before = g.openSet();
    for (const [nr, nc] of g.neighbors(r, c)) if (g.isMine(nr, nc)) g.at(S.API_RIGHT, nr, nc);
    g.at(entry, r, c);
    let expected = new Set(before);
    for (const [nr, nc] of g.neighbors(r, c)) if (!g.isMine(nr, nc)) expected = g.expectedOpen(nr, nc, expected);
    assert.deepEqual(g.openSet(), expected);
    assert.notEqual(g.state, STATE.LOST);
  }
});

test('opening around a number does nothing until its flags match, and loses with a wrong flag', () => {
  const g = new Game({ level: 1, seed: 0x7777 });
  g.at(S.API_LEFT, 8, 8);
  const [r, c] = numberBesideCovered(g);
  const before = g.openSet();
  g.at(S.API_MIDDLE, r, c);
  assert.deepEqual(g.openSet(), before, 'it opened with no flags');

  const mines = [...g.neighbors(r, c)].filter(([nr, nc]) => g.isMine(nr, nc));
  const safe = [...g.neighbors(r, c)].filter(([nr, nc]) => !g.isMine(nr, nc) && !g.isOpen(nr, nc));
  g.at(S.API_RIGHT, ...safe[0]);
  for (const m of mines.slice(1)) g.at(S.API_RIGHT, ...m);
  g.at(S.API_MIDDLE, r, c);
  assert.equal(g.state, STATE.LOST);
});

test('the mouse is found beside GeoRAM, beside an REU, or not at all', () => {
  const mouse = { dx: 0, dy: 0, buttons: 0 };
  const page = trPage(mouse);
  const reu = (o) => ((o & 0x1f) < 0x0b ? [0x10, 0, 0, ...id('TRM'), 0, 0, 0, 0, 0][o & 0x1f] : 0xff);
  const cases = [
    [{ 0xdf00: page }, 0xdf],
    [{ 0xdf00: reu, 0xde00: page }, 0xde],
    [{ 0xdf00: reu, 0xde00: () => 7 }, 0],
    [{}, 0],
  ];
  for (const [pages, expected] of cases) {
    const g = new Game();
    g.pages = pages;
    g.call(S.API_FINDMOUSE);
    assert.equal(g.ram[S.mregs + 1], expected);
  }
});

function mouseGame(level = 0) {
  const mouse = { dx: 0, dy: 0, buttons: 0 };
  const g = new Game({ level, pages: { 0xdf00: trPage(mouse) } });
  g.call(S.API_FINDMOUSE);
  return { g, mouse };
}

const pos = (g) => [g.ram[S.posLo] | (g.ram[S.posHi] << 8), g.ram[S.posLo + 1] | (g.ram[S.posHi + 1] << 8)];

function setPos(g, x, y) {
  g.ram[S.posLo] = x & 0xff;
  g.ram[S.posHi] = x >> 8;
  g.ram[S.posLo + 1] = y;
  g.ram[S.posHi + 1] = 0;
}

test('the mouse moves the pointer by signed steps and keeps it on the screen', () => {
  const { g, mouse } = mouseGame();
  setPos(g, 160, 100);
  Object.assign(mouse, { dx: 100, dy: -50 });
  g.call(S.API_READMOUSE);
  assert.deepEqual(pos(g), [260, 50]);
  Object.assign(mouse, { dx: 127, dy: 127 });
  g.call(S.API_READMOUSE);
  g.call(S.API_READMOUSE);
  assert.deepEqual(pos(g), [319, 199]);
  for (let i = 0; i < 4; i++) {
    Object.assign(mouse, { dx: -127, dy: -127 });
    g.call(S.API_READMOUSE);
  }
  assert.deepEqual(pos(g), [0, 0]);
});

test('a mouse button acts once when pressed, on the cell under the pointer', () => {
  const { g, mouse } = mouseGame();
  const spec = LEVELS[0];
  setPos(g, (spec.left + 2) * 8 + 1, (TOP + 3) * 8 + 6);
  mouse.buttons = 2;
  g.call(S.API_READMOUSE);
  assert.ok(g.isFlag(3, 2));
  g.call(S.API_READMOUSE);
  assert.ok(g.isFlag(3, 2), 'a held button flagged again');
  mouse.buttons = 0;
  g.call(S.API_READMOUSE);
  setPos(g, (spec.left + 6) * 8, (TOP + 6) * 8 + 7);
  mouse.buttons = 1;
  g.call(S.API_READMOUSE);
  assert.equal(g.state, STATE.PLAYING);
  assert.ok(g.isOpen(6, 6));
});

test('clicking a level label starts that level, and the face starts a new game', () => {
  const { g, mouse } = mouseGame();
  const click = (col, row) => {
    setPos(g, col * 8 + 2, row * 8 + 2);
    mouse.buttons = 1;
    g.call(S.API_READMOUSE);
    mouse.buttons = 0;
    g.call(S.API_READMOUSE);
  };
  click(35, 24);
  assert.deepEqual([g.rows, g.cols], [16, 30]);
  click(20, 24);
  assert.deepEqual([g.rows, g.cols], [16, 16]);
  click(4, 24);
  assert.deepEqual([g.rows, g.cols], [9, 9]);
  click(15 + 4, TOP + 4);
  assert.equal(g.state, STATE.PLAYING);
  click(20, 3);
  assert.equal(g.state, STATE.READY);
});

test('the keys pick a level and start a new game', () => {
  const g = new Game();
  const key = (petscii) => {
    g.ram.set([0xa9, petscii, 0x60], 0xffe4); // GETIN: lda #key, rts
    g.call(S.API_READKEYS);
  };
  key(0x87); // F5
  assert.deepEqual([g.rows, g.cols], [16, 30]);
  key(0x85); // F1
  assert.deepEqual([g.rows, g.cols], [9, 9]);
  for (const k of [0x4e, 0x0d]) { // N, Return
    g.at(S.API_LEFT, 4, 4);
    assert.equal(g.state, STATE.PLAYING);
    key(k);
    assert.equal(g.state, STATE.READY, `key $${k.toString(16)} did not start a new game`);
  }
});
