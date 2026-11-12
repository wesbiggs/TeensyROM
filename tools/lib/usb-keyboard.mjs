// SPDX-License-Identifier: MIT
//
// The "TeensyROM + USB keyboard" build (build-firmware.mjs --usb-keyboard): the stock
// Serial + MIDI device plus a boot-protocol keyboard on the Teensy's device port, which
// PETSKEY Remote types into (Source/Teensy/MinimalBoot/Common/IO_Handlers/UsbKbd.c).
//
// None of Teensyduino's USB types is Serial + MIDI + Keyboard, so the build adds one to
// usb_desc.h in its private copy of the core, never the installed one. usb_desc.c already
// lays a keyboard interface out after MIDI when KEYBOARD_INTERFACE is defined, and
// usb_keyboard.c compiles in with it.

// Picked with -D by build.usbtype; MinimalBoot.ino and IOHandlers.h accept it beside
// USB_MIDI_SERIAL, and the SwiftLink handler compiles UsbKbd.c only under it.
export const USB_KEYBOARD_TYPE = 'USB_MIDI_SERIAL_KEYBOARD';

// A different interface layout needs its own product ID: Windows keeps what it set up
// for 16C0:0489 per VID/PID, and would apply the Serial + MIDI setup to this device.
// 0x048D is unassigned in Teensyduino 1.60-1.62; PJRC has not allocated it. A build
// for release needs an ID PJRC agrees to.
export const USB_KEYBOARD_PRODUCT_ID = 0x048D;

const ANCHOR = '\n#elif defined(USB_MIDI4_SERIAL)\n';

// USB_MIDI_SERIAL's block (core 1.60-1.62), with the keyboard as interface 3 on
// endpoint 5. The name strings are TeensyROM's own (Common/UsbNames.c), so the
// defaults here are never shown.
const BLOCK = `
#elif defined(${USB_KEYBOARD_TYPE})
  #define VENDOR_ID		0x16C0
  #define PRODUCT_ID		0x${USB_KEYBOARD_PRODUCT_ID.toString(16).toUpperCase().padStart(4, '0')}
  #define MANUFACTURER_NAME	{'T','e','e','n','s','y','d','u','i','n','o'}
  #define MANUFACTURER_NAME_LEN	11
  #define PRODUCT_NAME		{'T','e','e','n','s','y',' ','M','I','D','I'}
  #define PRODUCT_NAME_LEN	11
  #define EP0_SIZE		64
  #define NUM_ENDPOINTS         5
  #define NUM_INTERFACE		4
  #define CDC_IAD_DESCRIPTOR	1
  #define CDC_STATUS_INTERFACE	0
  #define CDC_DATA_INTERFACE	1	// Serial
  #define CDC_ACM_ENDPOINT	2
  #define CDC_RX_ENDPOINT       3
  #define CDC_TX_ENDPOINT       3
  #define CDC_ACM_SIZE          16
  #define CDC_RX_SIZE_480       512
  #define CDC_TX_SIZE_480       512
  #define CDC_RX_SIZE_12        64
  #define CDC_TX_SIZE_12        64
  #define MIDI_INTERFACE        2	// MIDI
  #define MIDI_NUM_CABLES       1
  #define MIDI_TX_ENDPOINT      4
  #define MIDI_TX_SIZE_12       64
  #define MIDI_TX_SIZE_480      512
  #define MIDI_RX_ENDPOINT      4
  #define MIDI_RX_SIZE_12       64
  #define MIDI_RX_SIZE_480      512
  #define KEYBOARD_INTERFACE    3	// Keyboard
  #define KEYBOARD_ENDPOINT     5
  #define KEYBOARD_SIZE         8	// boot protocol
  #define KEYBOARD_INTERVAL     1
  #define ENDPOINT2_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT3_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT4_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT5_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
`;

// The keyboard type's block goes in front of USB_MIDI4_SERIAL's, in the chain of
// #elif defined(USB_...) blocks. Refused, not skipped, if the anchor has moved or the
// type is already there: either way the core is not the one this was written against.
export function patchUsbDescForKeyboard(usbDescSource) {
  const source = usbDescSource.replace(/\r\n/g, '\n');
  if (source.includes(`defined(${USB_KEYBOARD_TYPE})`)) {
    throw new Error(`usb_desc.h already defines ${USB_KEYBOARD_TYPE}; this core is not a stock one`);
  }
  if (source.split(ANCHOR).length !== 2) {
    throw new Error('usb_desc.h: "#elif defined(USB_MIDI4_SERIAL)" not found exactly once; ' +
      'the --usb-keyboard patch was written against Teensyduino 1.60-1.62');
  }
  const taken = source.match(new RegExp(`PRODUCT_ID\\s+0x0*${USB_KEYBOARD_PRODUCT_ID.toString(16)}\\b`, 'i'));
  if (taken) {
    throw new Error(`usb_desc.h already uses product ID 0x${USB_KEYBOARD_PRODUCT_ID.toString(16)} for another USB type`);
  }
  return source.replace(ANCHOR, BLOCK + ANCHOR);
}
