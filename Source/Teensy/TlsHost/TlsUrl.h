// SPDX-License-Identifier: MIT
// In a header of its own because the sketch preprocessor hoists TlsHost.ino's
// prototypes above any type that file defines.
#pragma once
#include <stdint.h>

// path holds a signed redirect target: a GitHub release asset's Location is
// about 900 bytes.
struct Url { char host[128]; uint16_t port; char path[2048]; };
