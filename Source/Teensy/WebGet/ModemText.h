// SPDX-License-Identifier: MIT
//
// The modem's conversions and checks that need nothing from the board, in a
// header of their own so Source/Teensy/tests/webget-modem.test.js can hold
// them to what they stand for.
#pragma once
#include <stdint.h>

// IOH_TeensyROM.c's ASCIItoPETSCII, entry for entry, as code: that file is the
// main image's TR menu handler, which no host links.
FLASHMEM static uint8_t HostToPETSCII(uint8_t c)
{
   c &= 0x7f;
   if (c < 32) return c == 9 ? 32 : (c == 10 || c == 13) ? c : 0;
   if (c >= 'A' && c <= 'Z') return c + 32;
   if (c >= 'a' && c <= 'z') return c - 32;
   switch (c)
   {
      case '\\': return 47;
      case '_':  return 164;
      case '`':  return 39;
      case '{':  return 179;
      case '|':  return 125;
      case '}':  return 171;
      case '~':  return 178;
      case 127:  return 95;
   }
   return c;
}

// A key the C64 sent, as ASCII with its case kept: unshifted letters are the
// lower case the C64's text screen shows them in. Everything but a letter or
// the left-arrow key comes out as the SwiftLink's own SwiftTxBufToLcaseASSCII
// makes it, so a dial string reads as the AT parser read it.
FLASHMEM static char CaseKept(uint8_t key)
{
   if (key >= 0x41 && key <= 0x5a) return key + 0x20;
   if (key >= 0xc1 && key <= 0xda) return key - 0x80;
   if (key == 0x5f) return '_';   // the left-arrow key
   key &= 0x7f;
   if (key & 0x40) key |= 0x20;
   return key;
}

// What DMA may write: from the screen up to the I/O area, which DMA would
// write instead of RAM. Below $0400 are the zero page, the stack and the
// KERNAL's and BASIC's pages, which a running BASIC is using.
static constexpr uint16_t PrgFloor = 0x0400, PrgCeiling = 0xD000;

// A .tls file's size, before it is read: empty is BASIC alone, and a program
// is a load address and at least one byte, no more than fits.
inline bool PrgSizeFits(uint32_t size) { return size == 0 || (size >= 3 && size <= PrgCeiling - PrgFloor + 2); }

// Its load address and length once read.
inline bool PrgPlaceFits(uint16_t load, uint32_t len) { return load >= PrgFloor && load + len <= PrgCeiling; }
