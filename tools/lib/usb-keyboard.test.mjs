// SPDX-License-Identifier: MIT
import test from 'node:test';
import assert from 'node:assert/strict';
import { patchUsbDescForKeyboard, USB_KEYBOARD_TYPE } from './usb-keyboard.mjs';

// The two blocks around the anchor, as Teensyduino 1.60-1.62 has them
const MIDI_SERIAL = `#elif defined(USB_MIDI_SERIAL)
  #define VENDOR_ID		0x16C0
  #define PRODUCT_ID		0x0489
  #define MANUFACTURER_NAME	{'T','e','e','n','s','y','d','u','i','n','o'}
  #define MANUFACTURER_NAME_LEN	11
  #define PRODUCT_NAME		{'T','e','e','n','s','y',' ','M','I','D','I'}
  #define PRODUCT_NAME_LEN	11
  #define EP0_SIZE		64
  #define NUM_ENDPOINTS         4
  #define NUM_INTERFACE		3
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
  #define ENDPOINT2_CONFIG	ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT
  #define ENDPOINT3_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK
  #define ENDPOINT4_CONFIG	ENDPOINT_RECEIVE_BULK + ENDPOINT_TRANSMIT_BULK

#elif defined(USB_MIDI4_SERIAL)
  #define VENDOR_ID		0x16C0
  #define PRODUCT_ID		0x0489
`;
const CORE = `#if defined(USB_SERIAL)\n  #define PRODUCT_ID		0x0483\n\n${MIDI_SERIAL}#endif\n`;

function defines(source, type) {
  const block = source.split(/^#elif defined\(|^#if defined\(/m).find((b) => b.startsWith(`${type})`));
  assert.ok(block, `no ${type} block`);
  return Object.fromEntries([...block.matchAll(/^\s*#define\s+(\w+)\s+(.*?)\s*(?:\/\/.*)?$/gm)].map((m) => [m[1], m[2]]));
}

test('the keyboard type is Serial + MIDI unchanged, plus a keyboard on interface 3, endpoint 5', () => {
  const patched = patchUsbDescForKeyboard(CORE);
  const stock = defines(patched, 'USB_MIDI_SERIAL');
  const keyboard = defines(patched, USB_KEYBOARD_TYPE);
  const changed = Object.keys(keyboard).filter((k) => keyboard[k] !== stock[k]).sort();
  assert.deepEqual(changed, ['ENDPOINT5_CONFIG', 'KEYBOARD_ENDPOINT', 'KEYBOARD_INTERFACE', 'KEYBOARD_INTERVAL',
    'KEYBOARD_SIZE', 'NUM_ENDPOINTS', 'NUM_INTERFACE', 'PRODUCT_ID']);
  assert.deepEqual(Object.keys(stock).filter((k) => !(k in keyboard)), []);
  assert.equal(keyboard.NUM_ENDPOINTS, '5');
  assert.equal(keyboard.NUM_INTERFACE, '4');
  assert.equal(keyboard.KEYBOARD_INTERFACE, '3');
  assert.equal(keyboard.KEYBOARD_SIZE, '8');
  assert.equal(keyboard.ENDPOINT5_CONFIG, 'ENDPOINT_RECEIVE_UNUSED + ENDPOINT_TRANSMIT_INTERRUPT');
  assert.equal(keyboard.PRODUCT_ID, '0x048D');
  // the stock types are untouched, and the chain still reads in order
  assert.equal(patched.replace(/#elif defined\(USB_MIDI_SERIAL_KEYBOARD\)[\s\S]*?\n\n/, ''), CORE);
});

test('a CRLF core patches the same', () => {
  assert.equal(patchUsbDescForKeyboard(CORE.replace(/\n/g, '\r\n')), patchUsbDescForKeyboard(CORE));
});

test('a core it was not written against is refused', () => {
  assert.throws(() => patchUsbDescForKeyboard(CORE.replace('USB_MIDI4_SERIAL', 'USB_MIDI4_SERIALX')), /not found exactly once/);
  assert.throws(() => patchUsbDescForKeyboard(patchUsbDescForKeyboard(CORE)), /already defines/);
  assert.throws(() => patchUsbDescForKeyboard(CORE.replace('0x0483', '0x048D')), /already uses product ID/);
});
