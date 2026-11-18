// SPDX-License-Identifier: MIT
// In a header of its own because the sketch preprocessor hoists TlsHost.ino's
// prototypes above any type that file defines.
#pragma once
#include <stdint.h>

struct Url { char host[128]; uint16_t port; char path[256]; };
