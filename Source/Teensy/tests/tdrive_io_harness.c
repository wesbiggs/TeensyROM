// SPDX-License-Identifier: MIT
// tdrive_io_harness drives the TDrive mailbox's fast paths (MinimalBoot/Common/tdrive/tdrive_io.c, included, not copied) the way
//   the TDrive KERNAL does, against a D64 held in memory on unit 0, device 8.
// tdrive_io_harness verify SIZE: with nothing mounted, VERIFY"BIG",8,1 of a SIZE-byte card file, loading at $0801 (a DMA host);
//   prints "read=<bytes the open read from the card> stat=<answer>".
// tdrive_io_harness save HOW: a fast SAVE of PROG whose device number moves between the open and the close (HOW: renumber = U0>
//   to device 10; scan = the bus scan releasing the number); prints "close=<answer> data=<DOS error> open=<channel 1 still open,
//   0/1> blocks=<PROG's size in blocks, or -1 if not on the disk>".
// tdrive_io_harness load: a fast LOAD of HELLO (600 bytes) whose device number moves to 10 after the open; prints
//   "bytes=<bytes the blocks gave> end=<0/1: a block said EOI>".
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../DiskImage/DiskImage.h"
#include "../MinimalBoot/Common/tdrive/tdrive.c"
#include "../MinimalBoot/Common/tdrive/tdrive_io.c"

static uint8_t Image[174848];
static d64_mem_t Mem;
static d64_t Disk;
static vio_t V;
static vio_hot_t H;

static uint8_t Command(uint8_t Cmd, uint8_t Arg, uint8_t Len)
{
   H.arg = Arg; H.len = Len;
   vio_io1_write(&H, VIO_CMD, Cmd);
   vio_poll(&V);
   return H.stat;
}

static uint8_t Named(uint8_t Cmd, const char *Name)
{
   memcpy(H.window, Name, strlen(Name));
   return Command(Cmd, 8, (uint8_t)strlen(Name));
}

static void Box(uint8_t Cmd, uint8_t Arg)
{
   uint8_t Data, St;
   tdrive_mailbox(&V.vd, Cmd, Arg, &Data, &St);
}

static void Renumber(void)   // OPEN 15,8,15,"U0>"+CHR$(10)
{
   const char U0[] = { 'U', '0', '>', 10 };
   Box(TD_LISTEN, 8);
   Box(TD_SECOND, 0x6F);
   for (size_t i = 0; i < sizeof U0; i++) Box(TD_CIOUT, (uint8_t)U0[i]);
   Box(TD_UNLSN, 0);
}

static void Mount(void)
{
   memset(Image, 0, sizeof Image);
   d64_mem_init(&Mem, Image, sizeof Image);
   if (d64_open_mem(&Disk, &Mem) || d64_format_blank(&Disk, "TEST", "TR")) exit(2);
   vio_init(&V, &H);
   tdrive_attach(&V.vd, 0, &Disk, 1);
   tdrive_set_devices(&V.vd, 0, 0);   // automatic: SD on 8
   vio_publish_devices(&V);
}

static int Blocks(const char *Name)
{
   d64_entry_t Entry;
   return d64_find(&Disk, Name, &Entry) == 1 ? Entry.blocks : -1;
}

// ---- verify: a plain card file, and a host that can read and write C64 RAM by DMA ----
static uint32_t PlainSize, PlainPos, CardBytes;
static uint8_t Ram[65536], DmaBuf[2048];
static int PlainOpen(void *c, int u, const char *n, uint32_t *size) { (void)c; (void)u; (void)n; *size = PlainSize; PlainPos = 0; return 0; }
static int PlainRead(void *c, int u, uint8_t *buf, uint16_t n)
{
   (void)c; (void)u;
   uint32_t k = PlainSize - PlainPos < n ? PlainSize - PlainPos : n;
   for (uint32_t i = 0; i < k; i++) buf[i] = PlainPos + i == 0 ? 0x01 : PlainPos + i == 1 ? 0x08 : 0;   // loads at $0801
   PlainPos += k; CardBytes += k;
   return (int)k;
}
static void PlainClose(void *c, int u) { (void)c; (void)u; }
static int DmaWrite(void *c, uint16_t a, const uint8_t *b, uint16_t n) { (void)c; memcpy(Ram + a, b, n); return 1; }
static int DmaRead(void *c, uint16_t a, uint8_t *b, uint16_t n) { (void)c; memcpy(b, Ram + a, n); return 1; }

int main(int argc, char **argv)
{
   if (argc >= 3 && !strcmp(argv[1], "verify"))
   {
      td_host_t Host;
      vio_init(&V, &H);
      memset(&Host, 0, sizeof Host);
      Host.plain_open = PlainOpen; Host.plain_read = PlainRead; Host.plain_close = PlainClose;
      tdrive_set_host(&V.vd, &Host);
      tdrive_attach(&V.vd, 0, NULL, 1);
      tdrive_set_devices(&V.vd, 0, 0);
      vio_publish_devices(&V);
      vio_set_dma(&V, DmaWrite, DmaRead, NULL, DmaBuf, sizeof DmaBuf);
      Ram[VIO_DMA_SA] = 1;
      PlainSize = (uint32_t)strtoul(argv[2], NULL, 0);
      uint8_t Stat = Named(VIO_FVERIFY_OPEN, "BIG");
      printf("read=%lu stat=%02x\n", (unsigned long)CardBytes, Stat);
      return 0;
   }
   if (argc >= 3 && !strcmp(argv[1], "save"))
   {
      Mount();
      if (Named(VIO_FSAVE_OPEN, "PROG") != VIO_OK || H.data) return 3;
      memset(H.window, 0x55, 100);
      Command(VIO_FSAVE_BLK, 0, 100);
      if (!strcmp(argv[2], "renumber")) Renumber();
      else Command(VIO_SCAN, 0, 0);
      uint8_t Stat = Command(VIO_FSAVE_CLOSE, 0, 0);
      printf("close=%02x data=%u open=%d blocks=%d\n", Stat, H.data, tdrive_chan_mode(&V.vd, 0, 1) != 0, Blocks("PROG"));
      return 0;
   }
   if (argc >= 2 && !strcmp(argv[1], "load"))
   {
      Mount();
      static uint8_t Prog[600];
      d64_wfile_t Out;
      if (d64_create(&Disk, "HELLO", 2, 0, &Out) || d64_write(&Disk, &Out, Prog, sizeof Prog) != (int)sizeof Prog ||
          d64_close_write(&Disk, &Out)) return 2;
      if (Named(VIO_FLOAD_OPEN, "HELLO") != VIO_OK || H.data) return 3;
      Renumber();
      unsigned long Bytes = 0;
      int End = 0;
      for (int Block = 0; Block < 100 && !End; Block++)   // 600 bytes are 5 blocks
      {
         Command(VIO_FLOAD_BLK, 0, 0);
         Bytes += H.data;
         End = (H.st & 0x40) != 0;
      }
      printf("bytes=%lu end=%d\n", Bytes, End);
      return 0;
   }
   return 1;
}
