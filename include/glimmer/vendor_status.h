// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
// Vendor status-page badges are not ported (glimmer's optional "showStatus" feature): always "operational".
#pragma once
#include <stdint.h>
#include "theme.h"
namespace VendorStatus {
    enum Vendor : uint8_t { CLAUDE = 0, OPENAI = 1, COUNT = 2 };
    inline const char* badge(Vendor) { return ""; }
    inline uint16_t badgeColor(Vendor) { return Theme::MUTED; }
}
