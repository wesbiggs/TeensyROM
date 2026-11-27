/* Build-time knobs shared by d64.c / tdrive.c / tdrive_io.c. */
#ifndef TD_CFG_H
#define TD_CFG_H
/* Placement for code that is NOT reachable from the bus interrupt. The firmware
 * glue defines this as FLASHMEM so only the O(1) register accessors stay in RAM1. The name stays VD_COLD: the shared
 * engine (TeensyROM's Source/Teensy/DiskImage, copied as d64.c/d64.h/DiskImage.h) uses it. */
#ifndef VD_COLD
#define VD_COLD
#endif

/* IEC device numbers of the two virtual drives (unit 0 = SD, unit 1 = USB). Any other device number
 * keeps using the real serial bus, so e.g. -DTD_DEV0=10 -DTD_DEV1=11 leaves physical drives 8 and 9
 * alone. The KERNAL image contains no device numbers: change them here and rebuild the firmware only. */
#ifndef TD_DEV0
#define TD_DEV0 8
#endif
#ifndef TD_DEV1
#define TD_DEV1 9
#endif

/* Simultaneously open data channels per unit. Real 1541s have 5; the pool is shared by
 * LOAD (channel 0), SAVE (1), and OPENs 2-14. Each slot costs ~290 bytes. */
#ifndef TD_NSLOT
#define TD_NSLOT 6
#endif
#endif
