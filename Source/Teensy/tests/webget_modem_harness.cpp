// SPDX-License-Identifier: MIT
// webget_modem_harness: the WebGet modem's ModemText.h, driven from stdin one command a line:
//   petscii            prints HostToPETSCII of 0..127, one decimal value a line
//   case HEX           prints CaseKept of the key HEX, as a decimal value
//   size N             prints 1 if PrgSizeFits takes a file of N bytes, else 0
//   place LOAD LEN     prints 1 if PrgPlaceFits takes the load address and length (hex), else 0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define FLASHMEM
#include "../WebGet/ModemText.h"

int main()
{
   char line[128];
   while (fgets(line, sizeof(line), stdin))
   {
      char cmd[16];
      if (sscanf(line, "%15s", cmd) != 1) continue;
      if (!strcmp(cmd, "petscii"))
      {
         for (unsigned c = 0; c < 128; c++) printf("%u\n", HostToPETSCII(c));
      }
      else if (!strcmp(cmd, "case"))
      {
         unsigned key;
         if (sscanf(line + 4, "%x", &key) != 1) return 2;
         printf("%u\n", (unsigned)(uint8_t)CaseKept(key));
      }
      else if (!strcmp(cmd, "size"))
      {
         unsigned long size;
         if (sscanf(line + 4, "%lu", &size) != 1) return 2;
         printf("%d\n", PrgSizeFits(size));
      }
      else if (!strcmp(cmd, "place"))
      {
         unsigned load, len;
         if (sscanf(line + 5, "%x %x", &load, &len) != 2) return 2;
         printf("%d\n", PrgPlaceFits(load, len));
      }
      else return 2;
   }
   return 0;
}
