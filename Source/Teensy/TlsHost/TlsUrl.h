// SPDX-License-Identifier: MIT
// In a header of its own because the sketch preprocessor hoists TlsHost.ino's
// prototypes above any type that file defines.
#pragma once
#include <stdint.h>

// path holds a signed redirect target: a GitHub release asset's Location is
// about 900 bytes.
struct Url { char host[128]; uint16_t port; char path[2048]; };

// A stage of the Stage enum in TlsHost.ino, and its value.
struct Result { uint8_t stage; uint32_t value; };

enum Outcome : uint8_t { Fetched, Redirected, Failed };
