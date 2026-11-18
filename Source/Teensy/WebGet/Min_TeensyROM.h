// SPDX-License-Identifier: MIT
//
// Build profile for the WebGet extension host.
//
// Deliberately close to Source/Teensy/VMBoot/Min_TeensyROM.h, because the
// difference is the point: what a host owes the build is this file and one
// .ino, and everything else it inherits from the minimal sketch it is overlaid
// onto. It defines FeatVMHost for the EasyFlash handler's two client-link
// hooks, VMHostIO2 and VMHostPoll, which WebGet.ino supplies itself; it does
// not include VMHost.h, so it compiles none of the module runtime.
//
// It must be built by tools/build-firmware.mjs, which generates the linker
// script that puts the image in the extension slot and reserves the module's
// memory windows:
//
//   node tools/build-firmware.mjs --target tr-plus --host-sketch Source/Teensy/WebGet --host-tls
//
// The stock IDE linker profile links for 0x60000000 and would produce an image
// the loader refuses (vm_host_slot_valid checks the boot base), so the guard
// below fails the build early rather than at install time.
#if !defined(VM_HOST_PROFILE) || !defined(USB_DISABLED)
#error "Build this image with tools/build-firmware.mjs --host-sketch"
#endif

#define MinimumBuild         //Must be defined for minimal build to identify in common files
#define FeatVMHost           //IO_Handler hooks for the client link, supplied by WebGet.ino

// No module runtime and no bank swapping: RAM_Image holds the 16 KiB WebGet
// client cartridge whole. Ethernet is the vendored NativeEthernet, brought in by
// the Net_*.cpp wrappers. IOH_EasyFlash still declares the swap buffers, 8 KiB
// of DTCM each, so this keeps the one it needs to compile: two would push the
// host's data into the module DTCM window.
#define Num8kSwapBuffers  1
#define EthernetDeduction 0
#define MaxRAM_ImageSize  16
