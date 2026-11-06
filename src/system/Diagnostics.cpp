// SPDX-License-Identifier: GPL-3.0-or-later
#include "system/Diagnostics.h"

#include <Arduino.h>
#include <user_interface.h>

#include <array>

#include "project_version.h"

namespace {
constexpr uint32_t MS_PER_SECOND = 1000U;
constexpr size_t MAX_CHECKPOINTS = 16;

struct Checkpoint {
    const __FlashStringHelper* name = nullptr;
    uint32_t freeHeap = 0;
    uint32_t maxBlock = 0;
};
std::array<Checkpoint, MAX_CHECKPOINTS> g_checkpoints{};
size_t g_checkpointCount = 0;
constexpr size_t HEX_TEXT_LEN = 12;
}

void Diagnostics::checkpoint(const __FlashStringHelper* name) {
    if (g_checkpointCount >= g_checkpoints.size()) {
        return;
    }
    g_checkpoints.at(g_checkpointCount++) = {name, ESP.getFreeHeap(),  // NOLINT(readability-static-accessed-through-instance)
                                             ESP.getMaxFreeBlockSize()};  // NOLINT(readability-static-accessed-through-instance)
}

auto Diagnostics::lastResetWasCrash() -> bool {
    const uint32_t reason = ESP.getResetInfoPtr()->reason;  // NOLINT(readability-static-accessed-through-instance)

    return reason == REASON_WDT_RST || reason == REASON_EXCEPTION_RST || reason == REASON_SOFT_WDT_RST;
}

void Diagnostics::fill(JsonDocument& doc) {
    doc["firmware"] = PROJECT_VER_STR;
    doc["uptime_s"] = millis() / MS_PER_SECOND;
    doc["free_heap"] = ESP.getFreeHeap();                    // NOLINT(readability-static-accessed-through-instance)
    doc["max_free_block"] = ESP.getMaxFreeBlockSize();           // NOLINT(readability-static-accessed-through-instance)
    doc["heap_fragmentation"] = ESP.getHeapFragmentation();  // NOLINT(readability-static-accessed-through-instance)
    doc["reset_reason"] = ESP.getResetReason();              // NOLINT(readability-static-accessed-through-instance)
    doc["last_reset_was_crash"] = lastResetWasCrash();

    if (g_checkpointCount > 0) {
        for (size_t i = 0; i < g_checkpointCount; i++) {
            doc["boot_heap"][i]["at"] = String(g_checkpoints.at(i).name);
            doc["boot_heap"][i]["free"] = g_checkpoints.at(i).freeHeap;
            doc["boot_heap"][i]["block"] = g_checkpoints.at(i).maxBlock;
        }
    }

    const rst_info* info = ESP.getResetInfoPtr();  // NOLINT(readability-static-accessed-through-instance)
    if (info->reason == REASON_EXCEPTION_RST) {
        std::array<char, HEX_TEXT_LEN> hex{};
        doc["exccause"] = info->exccause;
        snprintf(hex.data(), hex.size(), "0x%08x", info->epc1);
        doc["epc1"] = hex.data();
    }
}
