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

// Crash trace: the core calls custom_crash_callback() from the exception handler. It keeps the code-looking
// words of the stack (return addresses) in RTC memory, which survives the reset, so /system can report them
// and addr2line on firmware.elf turns them into function names. Block 100.. is clear of RescueMode's word.
constexpr uint32_t TRACE_RTC_OFFSET = 100;
constexpr uint32_t TRACE_MAGIC = 0x43524153;  // "CRAS"
constexpr size_t TRACE_WORDS = 22;
constexpr size_t TRACE_HEADER = 4;  // magic, count, free heap, Wi-Fi connect status
constexpr uint32_t CODE_LOW = 0x40200000;     // flash-mapped code
constexpr uint32_t CODE_HIGH = 0x40300000;
constexpr size_t TRACE_STACK_LIMIT = 400;     // words of stack to look through
}

extern "C" void custom_crash_callback(struct rst_info* info, uint32_t stack, uint32_t stackEnd) {
    std::array<uint32_t, TRACE_WORDS + TRACE_HEADER> trace{};
    trace.at(0) = TRACE_MAGIC;
    size_t count = 0;
    size_t looked = 0;
    for (uint32_t addr = stack; addr + sizeof(uint32_t) <= stackEnd && count < TRACE_WORDS && looked < TRACE_STACK_LIMIT;
         addr += sizeof(uint32_t), looked++) {
        const uint32_t word = *reinterpret_cast<const uint32_t*>(addr);  // NOLINT(performance-no-int-to-ptr)
        if (word >= CODE_LOW && word < CODE_HIGH) {
            trace.at(TRACE_HEADER + count++) = word;
        }
    }
    trace.at(1) = static_cast<uint32_t>(count);
    trace.at(2) = ESP.getFreeHeap();                        // NOLINT(readability-static-accessed-through-instance)
    trace.at(3) = static_cast<uint32_t>(wifi_station_get_connect_status());
    (void)info;
    ESP.rtcUserMemoryWrite(TRACE_RTC_OFFSET, trace.data(), sizeof(trace));  // NOLINT(readability-static-accessed-through-instance)
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
    doc[F("firmware")] = PROJECT_VER_STR;
    doc[F("uptime_s")] = millis() / MS_PER_SECOND;
    doc[F("free_heap")] = ESP.getFreeHeap();                    // NOLINT(readability-static-accessed-through-instance)
    doc[F("max_free_block")] = ESP.getMaxFreeBlockSize();           // NOLINT(readability-static-accessed-through-instance)
    doc[F("heap_fragmentation")] = ESP.getHeapFragmentation();  // NOLINT(readability-static-accessed-through-instance)
    doc[F("reset_reason")] = ESP.getResetReason();              // NOLINT(readability-static-accessed-through-instance)
    doc[F("last_reset_was_crash")] = lastResetWasCrash();

    if (g_checkpointCount > 0) {
        for (size_t i = 0; i < g_checkpointCount; i++) {
            doc[F("boot_heap")][i]["at"] = String(g_checkpoints.at(i).name);
            doc[F("boot_heap")][i][F("free")] = g_checkpoints.at(i).freeHeap;
            doc[F("boot_heap")][i][F("block")] = g_checkpoints.at(i).maxBlock;
        }
    }

    const rst_info* info = ESP.getResetInfoPtr();  // NOLINT(readability-static-accessed-through-instance)
    if (info->reason == REASON_EXCEPTION_RST) {
        std::array<char, HEX_TEXT_LEN> hex{};
        doc[F("exccause")] = info->exccause;
        snprintf(hex.data(), hex.size(), "0x%08x", info->epc1);
        doc[F("epc1")] = hex.data();
        snprintf(hex.data(), hex.size(), "0x%08x", info->excvaddr);
        doc[F("excvaddr")] = hex.data();

        std::array<uint32_t, TRACE_WORDS + TRACE_HEADER> trace{};
        if (ESP.rtcUserMemoryRead(TRACE_RTC_OFFSET, trace.data(), sizeof(trace)) &&  // NOLINT(readability-static-accessed-through-instance)
            trace.at(0) == TRACE_MAGIC) {
            doc[F("crash_free_heap")] = trace.at(2);
            doc[F("crash_wifi_status")] = trace.at(3);  // 0 idle, 1 connecting, 2 wrong password, 3 no AP, 4 failed, 5 got IP
            for (size_t i = 0; i < trace.at(1) && i < TRACE_WORDS; i++) {
                snprintf(hex.data(), hex.size(), "0x%08x", trace.at(TRACE_HEADER + i));
                doc[F("trace")][i] = hex.data();
            }
        }
    }
}
