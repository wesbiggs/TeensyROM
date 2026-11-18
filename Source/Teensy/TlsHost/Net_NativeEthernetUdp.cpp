// Compiles the vendored NativeEthernet into this host; see Common/NativeEthernet/README.md.
// MinimalBoot's own wrappers are empty under VM_HOST_PROFILE, so a host that wants
// Ethernet brings it in itself. DMAMEM would put the library's six socket-table pointers
// in .bss.dma, which the host linker script refuses: RAM2 is the module arena.
#include <Arduino.h>
#undef DMAMEM
#define DMAMEM
#include "Common/NativeEthernet/src/NativeEthernetUdp.cpp"
