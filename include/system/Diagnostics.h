// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ArduinoJson.h>

/**
 * @brief Runtime health information for remote debugging (there is no serial console on a deployed device)
 */
namespace Diagnostics {

/**
 * @brief Record the free heap at a named boot stage (shown as `boot_heap` by fill()). The name must be a flash
 *        string literal, F("..."), so it costs no RAM. At most 16 checkpoints are kept.
 */
void checkpoint(const __FlashStringHelper* name);

/**
 * @brief True if the previous reset was a crash (exception, hardware or software watchdog)
 */
auto lastResetWasCrash() -> bool;

/**
 * @brief Fill firmware version, uptime, heap numbers and the previous reset's reason.
 *
 * For exception resets `exccause` and `epc1` are included. `epc1` is the crashing instruction address and can be
 * decoded with `xtensa-lx106-elf-addr2line -e .pio/build/esp12e/firmware.elf <epc1>` against the matching build.
 */
void fill(JsonDocument& doc);

}  // namespace Diagnostics
