#!/usr/bin/env node
// SPDX-License-Identifier: MIT
//
// Build Source/Teensy/WebGet/ca.pem, the root certificates the WebGet host
// trusts, from any PEM bundle of roots:
//
//   node tools/webget-ca.mjs <bundle.pem> [out.pem]
//
// The bundle can be curl's cacert.pem, Linux's /etc/ssl/certs/ca-certificates.crt,
// or a macOS export (security find-certificate -a -p
// /System/Library/Keychains/SystemRootCertificates.keychain). Every root named
// below must be in it, or nothing is written.
//
// The list is short on purpose: the host refuses a ca.pem over 64 KiB, its
// mbedTLS takes only RSA up to 4096 bits and ECDSA up to P-384, and a file with
// one certificate it cannot parse is refused whole. These cover the authorities
// behind about 95% of websites by W3Techs' count (November 2026), each family's
// RSA and ECDSA roots where it has both.
import fs from 'node:fs';
import path from 'node:path';
import { X509Certificate } from 'node:crypto';
import { fileURLToPath } from 'node:url';

export const ROOTS = [
  { cn: 'ISRG Root X1' },                                    // Let's Encrypt
  { cn: 'ISRG Root X2' },
  { cn: 'USERTrust RSA Certification Authority' },           // Sectigo
  { cn: 'USERTrust ECC Certification Authority' },
  { cn: 'AAA Certificate Services' },                        // Comodo, cross-signing SSL.com
  { cn: 'GlobalSign Root CA' },                              // GlobalSign R1
  { cn: 'GlobalSign', ou: 'GlobalSign Root CA - R3' },
  { cn: 'Go Daddy Root Certificate Authority - G2' },        // GoDaddy
  { cn: 'Starfield Root Certificate Authority - G2' },
  { cn: 'DigiCert Global Root CA' },                         // DigiCert
  { cn: 'DigiCert Global Root G2' },
  { cn: 'GTS Root R1' },                                     // Google Trust Services
  { cn: 'GTS Root R4' },
  { cn: 'Amazon Root CA 1' },                                // Amazon
];

const field = (dn, key) => dn.split('\n').find((line) => line.startsWith(`${key}=`))?.slice(key.length + 1);

// The PEM blocks of a bundle, each with its parsed certificate.
export function certificates(text) {
  return [...text.matchAll(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g)]
    .map(([pem]) => ({ pem: pem + '\n', cert: new X509Certificate(pem) }));
}

// The named roots out of a bundle, in ROOTS order: self-signed, unexpired, and
// for a name issued more than once, the one that lasts longest.
export function selectRoots(text, now = new Date()) {
  const all = certificates(text);
  const missing = [];
  const chosen = ROOTS.map(({ cn, ou }) => {
    const matches = all.filter(({ cert }) => cert.subject === cert.issuer &&
      field(cert.subject, 'CN') === cn && (!ou || field(cert.subject, 'OU') === ou) &&
      new Date(cert.validTo) > now);
    matches.sort((a, b) => new Date(b.cert.validTo) - new Date(a.cert.validTo));
    if (!matches.length) missing.push(ou ? `${cn} (${ou})` : cn);
    return matches[0];
  });
  if (missing.length) throw new Error(`Not in the bundle, or expired: ${missing.join(', ')}`);
  return chosen;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [bundle, out = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'Source', 'Teensy', 'WebGet', 'ca.pem')] =
    process.argv.slice(2);
  if (!bundle) {
    console.error('usage: node tools/webget-ca.mjs <bundle.pem> [out.pem]');
    process.exit(2);
  }
  const roots = selectRoots(fs.readFileSync(bundle, 'utf8'));
  const text = roots.map(({ pem }) => pem).join('');
  fs.writeFileSync(out, text);
  for (const { cert } of roots) console.log(`${field(cert.subject, 'CN').padEnd(42)} until ${cert.validTo}`);
  console.log(`${roots.length} roots, ${text.length} bytes -> ${out}`);
}
