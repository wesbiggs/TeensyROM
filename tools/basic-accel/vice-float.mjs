// SPDX-License-Identifier: MIT
// Runs a C64 program in VICE and reports how long it takes: natively, or with BASIC's float
// routines answered by Source/Teensy/BasicAccel/CbmFloat.h the way a cartridge would.
//
//   node tools/basic-accel/vice-float.mjs [--program bench|sine] [--model native|tr-plus|tr|tr-plus-isr|all]
//        [--routines fadd,fsub,fmult,fdiv,fin] [--ntsc]
//
// VICE has no TeensyROM, so the cartridge is modeled through VICE's binary monitor. An exec
// checkpoint stops the C64 at a routine's entry; this script reads the zero page and registers,
// runs the C port, writes back what it computed, and points the PC at a run of NOPs ending in RTS that
// takes as many C64 cycles as the modeled cartridge would hold the C64 for (costFor, below). The
// C64's own clock, CIA2 in the benchmark, then measures the modeled run, and the benchmark's sum
// shows whether the results matched the ROM's. While the monitor holds it the C64 does not run,
// so the time this script takes costs the C64 nothing.
//
// The cycle costs are a model, not a measurement: see COSTS. Needs x64sc, the C64 ROMs VICE
// ships with, xvfb-run on a machine without a display, a C compiler, and ACME to build the
// benchmark (npm run build:c64 -- --project FMultBench).
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { commandExists } from '../lib/toolchain.mjs';
import { ROUTINES } from './float-cases.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const BENCH = path.join(root, 'Source/C64/FMultBench/build');
const HARNESS_SOURCE = path.join(root, 'Source/Teensy/tests/cbm_float_harness.c');
const VICE_ROMS = '/usr/share/vice/C64';
const VICE_SYMBOLS = path.join(VICE_ROMS, 'c64mem.sym');
const SLED = 0xc000;      // free RAM in the benchmark: NOPs from here, then RTS
const SLED_NOPS = 0x200;
const PAL_HZ = 985248;
const NTSC_HZ = 1022727;

// C64 cycles from the fetch at the routine's entry to the caller's next instruction, by model.
// All start the same way: the cartridge answers that fetch with JMP $DE00 (3 cycles), and the
// CPU runs on in IO1, where the cartridge serves what it executes.
//
// handshake: the IO1/ISR hand-off to thread mode and back. 75 is the 70-80 cycles
//   docs/Architecture/Constraints.md gives for a WriteC64Spans slice; an ISR-resident version
//   could be far less.
// tr-plus: the C64 held with /DMA while the TR reads what the routine reads and writes back the
//   bytes the result changed (it holds both copies), one cycle each, plus 4 to take and release
//   the bus. The arithmetic reads $61-$70 (16 bytes); FIN reads $68, CHRGET at $73-$8A to check
//   it is the ROM's (25), and the text bytes it used.
// tr: no bus mastering, so the CPU moves the bytes itself: LDA zp / STA $DExx (7 cycles) per
//   byte read, LDA #imm / STA zp (5) per byte written.
// tr-plus-isr: tr-plus with no thread-mode hand-off, as if the computation ran inside the DMA
//   cycles in the ISR. A lower bound, not a design: it is ~0.3 µs of work on the PHI2 path.
// Then all serve the registers the result determined: LDX/LDY/LDA #imm, CLC or SEC, CLV or
// BIT of a served $40 (2 cycles each, 4 for the BIT), and RTS (6).
export const COSTS = {
  'tr-plus': { handshake: 75, perRead: 1, perWrite: 1, busTurn: 4 },
  tr: { handshake: 75, perRead: 7, perWrite: 5, busTurn: 0 },
  'tr-plus-isr': { handshake: 0, perRead: 1, perWrite: 1, busTurn: 4 },
};
const readsFor = (routine, textUsed) => (routine === 'fin' ? 25 + textUsed : 16);

function costFor(model, set, reads, writes) {
  const c = COSTS[model];
  let served = 6;  // RTS
  if (set & 0x100) served += 2;
  if (set & 0x200) served += 2;
  if (set & 0x400) served += 2;
  if (set & 0x01) served += 2;
  if (set & 0x40) served += 3;  // CLV is 2, BIT is 4
  return 3 + c.handshake + c.busTurn + reads * c.perRead + writes * c.perWrite + served;
}

// ---- VICE binary monitor (API version 2) ----
class Monitor {
  constructor(socket) {
    this.socket = socket;
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.waiting = new Map();
    this.events = [];
    this.eventWaiters = [];
    socket.on('data', (chunk) => this.receive(chunk));
  }

  receive(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 12) {
      const length = this.buffer.readUInt32LE(2);
      if (this.buffer.length < 12 + length) return;
      const message = {
        type: this.buffer[6],
        error: this.buffer[7],
        id: this.buffer.readUInt32LE(8),
        body: this.buffer.subarray(12, 12 + length),
      };
      this.buffer = this.buffer.subarray(12 + length);
      const waiter = this.waiting.get(message.id);
      if (waiter && message.type === waiter.type) {
        this.waiting.delete(message.id);
        if (message.error) waiter.reject(new Error(`monitor command ${waiter.type.toString(16)} failed: error ${message.error}`));
        else waiter.resolve(message.body);
      }
      else if (message.id === 0xffffffff) {
        const take = this.eventWaiters.findIndex((w) => w.type === message.type);
        if (take >= 0) this.eventWaiters.splice(take, 1)[0].resolve(message.body);
        else this.events.push(message);
      }
    }
  }

  command(type, body = Buffer.alloc(0), responseType = type) {
    const id = this.nextId++;
    const header = Buffer.alloc(11);
    header[0] = 0x02;
    header[1] = 0x02;
    header.writeUInt32LE(body.length, 2);
    header.writeUInt32LE(id, 6);
    header[10] = type;
    return new Promise((resolve, reject) => {
      this.waiting.set(id, { type: responseType, resolve, reject });
      this.socket.write(Buffer.concat([header, body]));
    });
  }

  event(type) {
    const queued = this.events.findIndex((e) => e.type === type);
    if (queued >= 0) return Promise.resolve(this.events.splice(queued, 1)[0].body);
    return new Promise((resolve) => this.eventWaiters.push({ type, resolve }));
  }

  dropEvents() { this.events = []; }

  async registerIds() {
    const body = await this.command(0x83, Buffer.from([0]));
    const ids = {};
    for (let i = 0, at = 2; i < body.readUInt16LE(0); i++) {
      const size = body[at];
      ids[body.toString('latin1', at + 4, at + 4 + body[at + 3])] = body[at + 1];
      at += size + 1;
    }
    return ids;
  }

  async bankId(name) {
    const body = await this.command(0x82);
    for (let i = 0, at = 2; i < body.readUInt16LE(0); i++) {
      const size = body[at];
      if (body.toString('latin1', at + 4, at + 4 + body[at + 3]) === name) return body.readUInt16LE(at + 1);
      at += size + 1;
    }
    throw new Error(`VICE has no memory bank "${name}"`);
  }

  checkpoint(address, op) {
    const body = Buffer.alloc(8);
    body.writeUInt16LE(address, 0);
    body.writeUInt16LE(address, 2);
    body[4] = 1;  // stop
    body[5] = 1;  // enabled
    body[6] = op; // 1 load, 2 store, 4 exec
    body[7] = 0;
    return this.command(0x12, body, 0x11);
  }

  async read(start, end) {
    const body = Buffer.alloc(8);
    body[0] = 0;
    body.writeUInt16LE(start, 1);
    body.writeUInt16LE(end, 3);
    body[5] = 0;
    body.writeUInt16LE(this.bank, 6);
    const reply = await this.command(0x01, body);
    return reply.subarray(2, 2 + reply.readUInt16LE(0));
  }

  write(start, data) {
    const body = Buffer.alloc(8);
    body[0] = 0;
    body.writeUInt16LE(start, 1);
    body.writeUInt16LE(start + data.length - 1, 3);
    body[5] = 0;
    body.writeUInt16LE(this.bank, 6);
    return this.command(0x02, Buffer.concat([body, Buffer.from(data)]));
  }

  async registers() {
    const body = await this.command(0x31, Buffer.from([0]));
    const values = {};
    for (let i = 0, at = 2; i < body.readUInt16LE(0); i++) {
      values[body[at + 1]] = body.readUInt16LE(at + 2);
      at += body[at] + 1;
    }
    return values;
  }

  setRegisters(values) {
    const entries = Object.entries(values);
    const body = Buffer.alloc(3 + entries.length * 4);
    body[0] = 0;
    body.writeUInt16LE(entries.length, 1);
    entries.forEach(([id, value], i) => {
      body[3 + i * 4] = 3;
      body[4 + i * 4] = Number(id);
      body.writeUInt16LE(value, 5 + i * 4);
    });
    return this.command(0x32, body, 0x31);
  }

  resume() { return this.command(0xaa); }
  quit() { return this.command(0xbb).catch(() => {}); }
}

// ---- the C port, as a coprocess speaking the harness's line format ----
function startPort() {
  const exe = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'cbm-float-')), 'cbm_float_harness');
  const built = spawnSync('cc', ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror', '-o', exe, HARNESS_SOURCE], { encoding: 'utf8' });
  if (built.status !== 0) throw new Error(built.stderr);
  const child = spawn(exe, [], { stdio: ['pipe', 'pipe', 'inherit'] });
  let pending = '';
  const queue = [];
  child.stdout.on('data', (chunk) => {
    pending += chunk;
    let nl;
    while ((nl = pending.indexOf('\n')) >= 0) {
      const line = pending.slice(0, nl);
      pending = pending.slice(nl + 1);
      queue.shift()(line);
    }
  });
  return {
    run(routine, a, x, y, p, zp, text) {
      const h = (v) => v.toString(16).padStart(2, '0');
      return new Promise((resolve) => {
        queue.push((line) => {
          const [result, set, A, X, Y, P, z, used] = line.split(' ');
          resolve({ result, set: parseInt(set, 16), a: parseInt(A, 16), x: parseInt(X, 16), y: parseInt(Y, 16), p: parseInt(P, 16), zp: Buffer.from(z, 'hex'), used: Number(used) });
        });
        const textHex = text ? ` ${Buffer.from(text).toString('hex')}` : '';
        child.stdin.write(`${routine} ${h(a)} ${h(x)} ${h(y)} ${h(p)} ${Buffer.from(zp).toString('hex')}${textHex}\n`);
      });
    },
    stop() { child.stdin.end(); },
  };
}

function labels() {
  const text = fs.readFileSync(path.join(BENCH, 'FMultBench.labels'), 'utf8');
  return Object.fromEntries([...text.matchAll(/al C:([0-9a-f]+) \.(\w+)/gi)].map((m) => [m[2], parseInt(m[1], 16)]));
}

async function connect(port) {
  for (let i = 0; i < 100; i++) {
    try {
      return await new Promise((resolve, reject) => {
        const s = net.connect(port, '127.0.0.1', () => resolve(s));
        s.once('error', reject);
      });
    }
    catch { await new Promise((r) => setTimeout(r, 200)); }
  }
  throw new Error('VICE binary monitor did not answer');
}

// The programs this script can run: where each signals it has finished, and what it reports
const PROGRAMS = {
  bench: {
    prg: () => path.join(BENCH, 'FMultBench.prg'),
    done: () => labels().Results + 10,
    async finish(mon) {
      const at = labels().Results;
      const r = Buffer.from(await mon.read(at, at + 10));
      if (r[10] !== 1) return null;
      return { empty: r.readUInt32LE(0), full: r.readUInt32LE(4), check: r.readUInt16LE(8) };
    },
  },
  sine: {
    prg() {
      const out = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'sineplot-')), 'sineplot.prg');
      const built = spawnSync('petcat', ['-w2', '-o', out, '--', path.join(root, 'tools/basic-accel/SinePlot.bas')], { encoding: 'utf8' });
      if (built.status !== 0) throw new Error(`petcat: ${built.stderr}`);
      return out;
    },
    done: () => 828,
    async finish(mon) {
      const r = Buffer.from(await mon.read(828, 831));
      if (r[0] !== 1) return null;
      const bitmap = Buffer.from(await mon.read(0x2000, 0x3f3f));
      let check = 0;
      for (const b of bitmap) check = (check * 31 + b) >>> 0;
      return { jiffies: r.readUInt16LE(2), check: check & 0xffff };
    },
    sampling: async (mon) => (await mon.read(829, 829))[0] === 1,  // SinePlot.bas sets it as the plot starts
  },
};

// ---- a sampling profiler: the KERNAL's IRQ entry, 50 or 60 times a second ----
const IRQ_ENTRY = 0xff48;

function romSymbols() {
  const text = fs.readFileSync(VICE_SYMBOLS, 'utf8');
  return [...text.matchAll(/^al ([0-9a-f]{4}) \.(\w+)/gim)]
    .map((m) => ({ at: parseInt(m[1], 16), name: m[2].replace(/^[bk]/, '') }))
    .filter((s) => s.at >= 0xa000 && s.at < 0xc000 || s.at >= 0xe000)
    .sort((a, b) => a.at - b.at);
}

function makeProfiler() {
  const rom = { basic: fs.readFileSync(path.join(VICE_ROMS, 'basic-901226-01.bin')), kernal: fs.readFileSync(path.join(VICE_ROMS, 'kernal-901227-03.bin')) };
  const romByte = (a) => (a >= 0xa000 && a < 0xc000 ? rom.basic[a - 0xa000] : a >= 0xe000 ? rom.kernal[a - 0xe000] : undefined);
  const symbols = romSymbols();
  const where = (pc) => {
    if (pc >= SLED && pc <= SLED + SLED_NOPS) return '(cartridge)';
    if (pc >= 0x73 && pc < 0x8b) return 'CHRGET (RAM)';
    if (romByte(pc) === undefined) return '(RAM)';
    let name = '(ROM)';
    for (const s of symbols) { if (s.at > pc) break; name = s.name; }
    return name;
  };
  const self = new Map();
  const total = new Map();
  let samples = 0;
  return {
    // stack: page 1, sp: the stack pointer at IRQ_ENTRY. The hardware pushed P, then the PC it
    // interrupted; above them, any address whose byte two below is a JSR in ROM is taken as a
    // return address, and the routine that JSR called as active. BASIC's own stack frames can
    // pass that test by chance, so the totals are a guide.
    sample(stack, sp) {
      samples++;
      const pc = stack[(sp + 2) & 0xff] | stack[(sp + 3) & 0xff] << 8;
      const here = where(pc);
      self.set(here, (self.get(here) ?? 0) + 1);
      const active = new Set([here]);
      for (let i = sp + 4; i < 0xff; i++) {
        const ret = stack[i] | stack[i + 1] << 8;
        if (romByte(ret - 2) === 0x20) active.add(where(romByte(ret - 1) | romByte(ret) << 8));
      }
      for (const name of active) total.set(name, (total.get(name) ?? 0) + 1);
    },
    report(limit = 25) {
      const pct = (n) => `${((100 * n) / samples).toFixed(1).padStart(5)}%`;
      const rows = [...total.entries()].sort((a, b) => (self.get(b[0]) ?? 0) - (self.get(a[0]) ?? 0) || b[1] - a[1]).slice(0, limit);
      const lines = [`${samples} samples. self: time in the routine itself; total: with what it called.`];
      for (const [name, n] of rows) lines.push(`  ${name.padEnd(14)} self ${pct(self.get(name) ?? 0)}   total ${pct(n)}`);
      return lines.join('\n');
    },
  };
}

async function runModel(model, program, { ntsc, routines, profiler }) {
  const port = 6510 + Math.floor(Math.random() * 1000);
  const vice = ['x64sc', '-default', '-warp', '-sounddev', 'dummy', ntsc ? '-ntsc' : '-pal',
    '-binarymonitor', '-binarymonitoraddress', `ip4://127.0.0.1:${port}`,
    '-autostartprgmode', '1', '-autostart', program.prg()];
  const [cmd, ...args] = process.env.DISPLAY || !commandExists('xvfb-run') ? vice : ['xvfb-run', '-a', ...vice];
  const child = spawn(cmd, args, { stdio: 'ignore' });
  const mon = new Monitor(await connect(port));
  const port_ = model === 'native' ? null : startPort();
  try {
    mon.bank = await mon.bankId('cpu');
    const reg = await mon.registerIds();
    await mon.checkpoint(program.done(), 2);
    const hooks = new Map();
    if (port_) {
      await mon.write(SLED, [...Array(SLED_NOPS).fill(0xea), 0x60]);
      for (const routine of routines) {
        hooks.set(ROUTINES[routine], routine);
        await mon.checkpoint(ROUTINES[routine], 4);
      }
    }
    if (profiler) await mon.checkpoint(IRQ_ENTRY, 4);
    const stats = { hooked: 0, native: 0, modeled: 0, calls: {} };
    mon.dropEvents();
    await mon.resume();
    for (;;) {
      const stop = await mon.event(0x62);
      const pc = stop.readUInt16LE(0);
      const routine = hooks.get(pc);
      if (profiler && pc === IRQ_ENTRY) {
        if (await program.sampling(mon)) {
          const r = await mon.registers();
          profiler.sample(Buffer.from(await mon.read(0x100, 0x1ff)), r[reg.SP]);
        }
        mon.dropEvents();
        await mon.resume();
        continue;
      }
      if (!routine) {
        const result = await program.finish(mon);
        if (result) return { model, ...result, stats };
        mon.dropEvents();
        await mon.resume();
        continue;
      }
      const r = await mon.registers();
      const zp = await mon.read(0, 0xff);
      let out;
      for (const window of routine === 'fin' ? [32, 256] : [0]) {
        const txtPtr = zp[0x7a] | zp[0x7b] << 8;
        const text = window ? await mon.read(txtPtr, Math.min(0xffff, txtPtr + window - 1)) : null;
        out = await port_.run(routine, r[reg.A], r[reg.X], r[reg.Y], r[reg.FL], zp, text);
        if (out.result !== 'more') break;
      }
      if (out.result !== 'done') {
        stats.native++;
        mon.dropEvents();
        await mon.resume();
        continue;
      }
      const changed = [];
      for (let at = 0; at < 256; at++) if (out.zp[at] !== zp[at]) changed.push(at);
      const cost = costFor(model, out.set, readsFor(routine, out.used), changed.length);
      const nops = Math.max(0, Math.round((cost - 6) / 2));
      stats.hooked++;
      stats.calls[routine] = (stats.calls[routine] ?? 0) + 1;
      stats.modeled += 6 + nops * 2;
      for (const at of changed) await mon.write(at, [out.zp[at]]);
      await mon.setRegisters({ [reg.A]: out.a, [reg.X]: out.x, [reg.Y]: out.y, [reg.FL]: out.p, [reg.PC]: SLED + SLED_NOPS - nops });
      mon.dropEvents();
      await mon.resume();
    }
  }
  finally {
    port_?.stop();
    await mon.quit();
    mon.socket.destroy();
    child.kill();
  }
}

const argv = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : fallback;
};
const models = opt('--model', 'all') === 'all' ? ['native', ...Object.keys(COSTS)] : [opt('--model')];
const programName = opt('--program', 'bench');
const program = PROGRAMS[programName];
const ntsc = argv.includes('--ntsc');
const routines = opt('--routines', Object.keys(ROUTINES).join(',')).split(',');
const profile = argv.includes('--profile');
const hz = ntsc ? NTSC_HZ : PAL_HZ;
const COUNT = 4500;  // FMultBench.asm's Count

if (!program || models.some((m) => m !== 'native' && !COSTS[m]) || routines.some((r) => !ROUTINES[r]) ||
    (profile && !program.sampling)) {
  console.error('Usage: node tools/basic-accel/vice-float.mjs [--program bench|sine] [--model native|tr-plus|tr|tr-plus-isr|all]');
  console.error('         [--routines fadd,fsub,fmult,fdiv,fin] [--profile (sine only)] [--ntsc]');
  process.exit(2);
}
if (!commandExists('x64sc')) {
  console.error('x64sc (VICE) is not on PATH.');
  process.exit(1);
}
if (programName === 'bench' && !fs.existsSync(program.prg())) {
  console.error('Build the benchmark first: npm run build:c64 -- --project FMultBench');
  process.exit(1);
}

let native;
for (const model of models) {
  const profiler = profile ? makeProfiler() : null;
  const r = await runModel(model, program, { ntsc, routines, profiler });
  const line = [model.padEnd(11)];
  if (programName === 'bench') {
    const fmult = r.full - r.empty;
    line.push(`pass ${(r.full / hz).toFixed(2)} s`, `FMULT ${(fmult / COUNT).toFixed(1)} cycles/call`);
  }
  else line.push(`plot ${(r.jiffies / 60).toFixed(1)} s`);
  line.push(`check $${r.check.toString(16).padStart(4, '0')}`);
  if (model === 'native') native = r;
  else {
    const calls = Object.entries(r.stats.calls).map(([n, c]) => `${n} ${c}`).join(', ');
    line.push(`hooked ${calls} (model ${(r.stats.modeled / r.stats.hooked).toFixed(0)} cycles each), handed back ${r.stats.native}`);
    if (native && programName === 'bench') {
      line.push(`${((native.full - native.empty) / (r.full - r.empty)).toFixed(1)}x on FMULT, ${(native.full / r.full).toFixed(1)}x on the pass`);
    }
    if (native && programName === 'sine') line.push(`${(native.jiffies / r.jiffies).toFixed(2)}x on the plot`);
    if (native) line.push(r.check === native.check ? 'same result' : 'RESULT DIFFERS');
  }
  console.log(line.join('  |  '));
  if (profiler) console.log(profiler.report());
}
