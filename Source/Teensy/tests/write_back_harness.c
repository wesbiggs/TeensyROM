// SPDX-License-Identifier: MIT
// write_back_harness: drives the disk image write-back policy from stdin, one step a line, printing each answer:
//   wrote MS        a sector was written at MS
//   due MS          prints 1 if a sync is due at MS
//   sync OK MS      a sync at MS reached the card (OK 1) or not (OK 0); prints 1 if that failure is the one to report
//   dirty           prints 1 while writes have not reached the card
//   forget          the image was closed
#include <stdio.h>
#include <string.h>

#include "../DiskImage/WriteBack.h"

int main(void)
{
   struct ImageWriteBack Back = { 0, false, false };
   char Line[64];
   unsigned Ms, Ok;

   while (fgets(Line, sizeof Line, stdin))
   {
      if (sscanf(Line, "wrote %u", &Ms) == 1) ImageWriteBackWrote(&Back, Ms);
      else if (sscanf(Line, "due %u", &Ms) == 1) printf("%d\n", ImageWriteBackDue(&Back, Ms));
      else if (sscanf(Line, "sync %u %u", &Ok, &Ms) == 2) printf("%d\n", ImageWriteBackSynced(&Back, Ok, Ms));
      else if (!strncmp(Line, "dirty", 5)) printf("%d\n", Back.Dirty);
      else if (!strncmp(Line, "forget", 6)) ImageWriteBackForget(&Back);
   }
   return 0;
}
