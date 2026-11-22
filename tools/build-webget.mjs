#!/usr/bin/env node
// SPDX-License-Identifier: MIT
//
// Build the WebGet extension host and everything a card needs to run it:
//
//   npm run build:webget [-- [--label <name>] [--ccache]]
//
// writes build/webget/WebGet_<label>/, laid out as it goes on the SD card, and
// WebGet_<label>.zip beside it:
//
//   TeensyROM+_<ver>_WebGet.TRH     the host; install it from the TR menu
//   VMS/WWW/                         the package that claims .www and .tls: its
//                                    manifest, WebGet client cartridge, ca.pem,
//                                    and the module the package format requires,
//                                    which this host never runs
//   WebGet.www                       an empty list, which opens the prompt
//   basic.tls                        empty: BASIC with the SwiftLink modem
//   tlsterm.tls                      a dumb terminal for the modem
//   README.txt
//
// The label defaults to `git describe` against WebGet_v* tags. --ccache is passed to
// the firmware build.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { scanArgs } from './lib/cli-args.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.join(root, 'build', 'webget');
const hostDir = path.join(root, 'Source', 'Teensy', 'WebGet');

const { option, flag } = scanArgs(process.argv.slice(2), { options: ['--label'], flags: ['--ccache'] });

function run(command, args) {
  console.log(`\n$ ${command} ${args.join(' ')}`);
  const result = spawnSync(command, args, { cwd: root, stdio: 'inherit' });
  if (result.status !== 0) throw new Error(`${command} failed (${result.error ?? `exit ${result.status}`})`);
}

function describe() {
  const result = spawnSync('git', ['describe', '--tags', '--match', 'WebGet_v*', '--always', '--dirty'],
    { cwd: root, encoding: 'utf8' });
  if (result.status !== 0) throw new Error('git describe failed; pass --label');
  return result.stdout.trim().replace(/^WebGet_/, '');
}

const label = option('--label', null) ?? describe();
if (!/^[\w.+-]+$/.test(label)) throw new Error(`--label ${label}: letters, digits and . _ + - only`);
const stage = path.join(out, `WebGet_${label}`);
const zip = `${stage}.zip`;
fs.rmSync(stage, { recursive: true, force: true });
fs.rmSync(zip, { force: true });

run(process.execPath, ['tools/build-firmware.mjs', '--target', 'tr-plus', '--host-sketch', 'Source/Teensy/WebGet',
  '--host-tls', '--out', out, '--force', ...(flag('--ccache') ? ['--ccache'] : [])]);
const trh = fs.readdirSync(out).filter((name) => name.endsWith('_WebGet.TRH'))
  .map((name) => path.join(out, name)).sort((a, b) => fs.statSync(b).mtimeMs - fs.statSync(a).mtimeMs)[0];
if (!trh) throw new Error(`no *_WebGet.TRH in ${out}`);

const packageOut = path.join(out, 'package');
fs.rmSync(packageOut, { recursive: true, force: true });
run(process.execPath, ['tools/build-extension.mjs', '--id', 'WWW', '--extensions', 'www,tls', '--services', '0',
  '--source', 'vm/hello/hello.cpp', '--client-source', 'Source/C64/WebGet/webget.a', '--out', packageOut]);

fs.mkdirSync(path.join(stage, 'VMS'), { recursive: true });
fs.cpSync(path.join(packageOut, 'VMS', 'WWW'), path.join(stage, 'VMS', 'WWW'), { recursive: true });
fs.copyFileSync(path.join(hostDir, 'ca.pem'), path.join(stage, 'VMS', 'WWW', 'ca.pem'));
fs.copyFileSync(trh, path.join(stage, path.basename(trh)));
fs.writeFileSync(path.join(stage, 'WebGet.www'), '');
fs.writeFileSync(path.join(stage, 'basic.tls'), '');
run('acme', ['-f', 'cbm', '-o', path.join(stage, 'tlsterm.tls'), path.join(root, 'Source', 'C64', 'WebGet', 'tlsterm.a')]);
fs.copyFileSync(path.join(hostDir, 'README.txt'), path.join(stage, 'README.txt'));

// Entries are stored relative to the stage, so the zip unpacks onto a card root as is.
const zipped = spawnSync('zip', ['-r', '-X', '-q', zip, '.'], { cwd: stage, stdio: 'inherit' });
if (zipped.status !== 0) throw new Error(`zip failed (${zipped.error ?? `exit ${zipped.status}`})`);

console.log(`\nWebGet ${label}:\n  ${path.relative(root, stage)}/\n  ${path.relative(root, zip)}`);
