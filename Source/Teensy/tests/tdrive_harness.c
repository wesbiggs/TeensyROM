// SPDX-License-Identifier: MIT
// tdrive_harness drives the TDrive engine (MinimalBoot/Common/tdrive/tdrive.c, included, not copied) through its mailbox, as the
//   KERNAL's serial routines do, against a D64 held in memory with unit 0 on device 8.
// tdrive_harness load: saves a 600-byte program, LOADs it back through channel 0; prints "bytes=<n> same=<0|1> eoi=<0|1> status=<text>".
// tdrive_harness missing: LOADs a name that is not on the disk; prints "bytes=<n> status=<text>".
// tdrive_harness open SPEC NAME: OPENs SPEC on channel 3, as OPEN 3,8,3,"SPEC" does, on a disk that holds the program HELLO;
//   prints "status=<text> type=<DOS file type of NAME afterward, or none>".
// tdrive_harness scratch COMMAND: sends COMMAND on channel 15 to a disk that holds HELLO and HELLO2, then prints
//   "status=<text> files=<number left>".
// tdrive_harness alias NAME: prints "<vd_alias return> <alias>".
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../DiskImage/DiskImage.h"
#include "../MinimalBoot/Common/tdrive/tdrive.c"

static uint8_t Image[174848];
static d64_mem_t Mem;
static d64_t Disk;
static tdrive_t Drive;

static void Box(uint8_t Cmd, uint8_t Arg)
{
   uint8_t Data, St;
   tdrive_mailbox(&Drive, Cmd, Arg, &Data, &St);
}

static void Send(uint8_t Channel, const char *Bytes, size_t Len)   // LISTEN, SECOND, the bytes, UNLSN
{
   Box(VD_LISTEN, 8);
   Box(VD_SECOND, 0xF0 | Channel);
   for (size_t i = 0; i < Len; i++) Box(VD_CIOUT, (uint8_t)Bytes[i]);
   Box(VD_UNLSN, 0);
}

static void Status(char *Out, size_t Cap)   // the error channel: TALK, TKSA 15, bytes up to the CR
{
   uint8_t Data, St;
   size_t n = 0;
   Box(VD_TALK, 8);
   Box(VD_TKSA, 0x6F);
   do {
      tdrive_mailbox(&Drive, VD_ACPTR, 0, &Data, &St);
      if (Data != '\r' && n + 1 < Cap) Out[n++] = (char)Data;
   } while (!(St & 0x40) && n < Cap);
   Out[n] = 0;
   Box(VD_UNTLK, 0);
}

static int Setup(void)
{
   memset(Image, 0, sizeof Image);
   d64_mem_init(&Mem, Image, sizeof Image);
   if (d64_open_mem(&Disk, &Mem) || d64_format_blank(&Disk, "TEST", "TR")) return 1;
   tdrive_init(&Drive);
   tdrive_attach(&Drive, 0, &Disk, 1);
   tdrive_set_devices(&Drive, 8, 0);
   return 0;
}

static int Save(const char *Name, const uint8_t *Data, size_t Size)
{
   d64_wfile_t Out;
   if (d64_create(&Disk, Name, 2, 0, &Out) || d64_write(&Disk, &Out, Data, Size) != (int)Size) return 1;
   return d64_close_write(&Disk, &Out);
}

static int FileType(const char *Name)
{
   d64_entry_t Entry;
   return d64_find(&Disk, Name, &Entry) == 1 ? (Entry.type & 7) : -1;
}

static int Files(void)
{
   d64_dir_t It;
   d64_entry_t Entry;
   int n = 0;
   if (d64_dir_first(&Disk, &It)) return -1;
   while (d64_dir_next(&Disk, &It, &Entry) > 0) n += Entry.type != 0;
   return n;
}

static size_t Load(const char *Name, uint8_t *Buf, size_t Cap, int *Eoi)   // OPEN 0,8,0,name; the bytes through TALK / TKSA $60
{
   uint8_t Data, St;
   size_t n = 0;
   *Eoi = 0;
   Send(0, Name, strlen(Name));
   Box(VD_TALK, 8);
   Box(VD_TKSA, 0x60);
   while (n < Cap) {
      tdrive_mailbox(&Drive, VD_ACPTR, 0, &Data, &St);
      if (St & 0x02) break;   // timeout: nothing to read on the channel
      Buf[n++] = Data;
      if (St & 0x40) { *Eoi = 1; break; }
   }
   Box(VD_UNTLK, 0);
   return n;
}

int main(int argc, char **argv)
{
   char Text[64];
   if (argc < 2 || Setup()) return 2;
   if (!strcmp(argv[1], "load") || !strcmp(argv[1], "missing")) {
      uint8_t Program[600], Back[700];
      int Eoi;
      for (int i = 0; i < (int)sizeof Program; i++) Program[i] = (uint8_t)(i * 7 + 1);
      if (Save("HELLO", Program, sizeof Program)) return 2;
      bool Missing = !strcmp(argv[1], "missing");
      size_t n = Load(Missing ? "NOPE" : "HELLO", Back, sizeof Back, &Eoi);
      Status(Text, sizeof Text);
      if (Missing) printf("bytes=%zu status=%s\n", n, Text);
      else printf("bytes=%zu same=%d eoi=%d status=%s\n", n, n == sizeof Program && !memcmp(Back, Program, n), Eoi, Text);
      return 0;
   }
   if (!strcmp(argv[1], "open") && argc == 4) {
      uint8_t Program[10] = { 1, 2, 3 };
      if (Save("HELLO", Program, sizeof Program)) return 2;
      Send(3, argv[2], strlen(argv[2]));
      Status(Text, sizeof Text);
      int Type = FileType(argv[3]);
      printf("status=%s type=%s\n", Text, Type < 0 ? "none" : DIRTYPES[Type]);
      return 0;
   }
   if (!strcmp(argv[1], "scratch") && argc == 3) {
      uint8_t Program[10] = { 1, 2, 3 };
      if (Save("HELLO", Program, sizeof Program) || Save("HELLO2", Program, sizeof Program)) return 2;
      Send(15, argv[2], strlen(argv[2]));
      Status(Text, sizeof Text);
      printf("status=%s files=%d\n", Text, Files());
      return 0;
   }
   if (!strcmp(argv[1], "alias") && argc == 3) {
      char Out[17];
      int r = vd_alias(argv[2], Out);
      printf("%d %s\n", r, Out);
      return 0;
   }
   return 2;
}
