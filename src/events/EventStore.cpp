// SPDX-License-Identifier: GPL-3.0-or-later
#include "events/EventStore.h"

#include <LittleFS.h>
#include <Logger.h>

#include <cstddef>

namespace {
constexpr const char* LOG_TAG = "Events";
constexpr const char* LIVE_FILE = "/events.bin";
constexpr const char* TEMP_FILE = "/events.tmp";
constexpr const char* STAGING_FILE = "/events.new";
constexpr const char* QUARANTINE_FILE = "/events.bad";
constexpr uint32_t MAGIC = 0x31545645;  // "EVT1"
constexpr uint16_t VERSION = 1;

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t recordSize;
    uint16_t count;
    uint16_t reserved;
};

size_t g_count = 0;
uint32_t g_revision = 1;
size_t g_staged = 0;  // records appended to STAGING_FILE so far
bool g_staging = false;

auto recordOffset(size_t slot) -> size_t { return sizeof(Header) + (slot * sizeof(Events::Record)); }

auto makeHeader(size_t count) -> Header {
    return Header{MAGIC, VERSION, static_cast<uint16_t>(sizeof(Events::Record)), static_cast<uint16_t>(count), 0};
}

auto headerValid(const Header& header, size_t fileSize) -> bool {
    return header.magic == MAGIC && header.version == VERSION && header.recordSize == sizeof(Events::Record) &&
           header.count <= Events::MAX_EVENTS && fileSize == recordOffset(header.count);
}

auto writeHeader(File& file, size_t count) -> bool {
    const Header header = makeHeader(count);

    return file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header);
}

auto writeRecord(File& file, const Events::Record& record) -> bool {
    return file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
}

auto readRecord(File& file, Events::Record& out) -> bool {
    return file.read(reinterpret_cast<uint8_t*>(&out), sizeof(out)) == sizeof(out);
}

/** @brief Swap a finished temp file over the live file. */
auto commitFile(const char* tmpPath, size_t count) -> bool {
    LittleFS.remove(LIVE_FILE);
    if (!LittleFS.rename(tmpPath, LIVE_FILE)) {
        Logger::error(F("Could not install the new event file"), LOG_TAG);

        return false;
    }
    g_count = count;
    g_revision++;

    return true;
}

/**
 * @brief Rewrite the live file through a temp file. `keep` is called for every existing record: return false to drop
 *        it, or edit it in place. `extra`, if not null, is appended (an insert).
 */
template <class Keep>
auto rewrite(Keep keep, const Events::Record* extra) -> EventStore::Result {
    File source = LittleFS.open(LIVE_FILE, "r");
    File out = LittleFS.open(TEMP_FILE, "w");
    if (!out) {
        return EventStore::Result::IoError;
    }
    size_t written = 0;
    bool success = writeHeader(out, 0);  // fixed up below
    if (source) {
        source.seek(sizeof(Header));
        Events::Record record;
        for (size_t slot = 0; success && slot < g_count && readRecord(source, record); slot++) {
            if (keep(slot, record)) {
                success = writeRecord(out, record);
                written++;
            }
        }
    }
    if (success && extra != nullptr) {
        if (written >= Events::MAX_EVENTS) {
            out.close();
            LittleFS.remove(TEMP_FILE);

            return EventStore::Result::Full;
        }
        success = writeRecord(out, *extra);
        written++;
    }
    if (success) {
        out.seek(0);
        success = writeHeader(out, written);
    }
    out.close();
    if (source) {
        source.close();
    }
    if (!success || !commitFile(TEMP_FILE, written)) {
        LittleFS.remove(TEMP_FILE);

        return EventStore::Result::IoError;
    }

    return EventStore::Result::Ok;
}
}  // namespace

void EventStore::begin() {
    File file = LittleFS.open(LIVE_FILE, "r");
    LittleFS.remove(TEMP_FILE);
    LittleFS.remove(STAGING_FILE);
    g_staging = false;
    if (!file) {
        g_count = 0;

        return;
    }
    Header header{};
    const bool read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header);
    const bool valid = read && headerValid(header, file.size());
    file.close();
    if (!valid) {
        Logger::warn(F("Event file damaged or from another build: moved aside, schedule is empty"), LOG_TAG);
        LittleFS.remove(QUARANTINE_FILE);
        LittleFS.rename(LIVE_FILE, QUARANTINE_FILE);
        g_count = 0;

        return;
    }
    g_count = header.count;
}

auto EventStore::count() -> size_t { return g_count; }

auto EventStore::revision() -> uint32_t { return g_revision; }

auto EventStore::read(size_t slot, Events::Record& out) -> bool {
    if (slot >= g_count) {
        return false;
    }
    File file = LittleFS.open(LIVE_FILE, "r");
    if (!file || !file.seek(recordOffset(slot))) {
        return false;
    }
    const bool success = readRecord(file, out);
    file.close();

    return success;
}

auto EventStore::find(const char* eventId, Events::Record* out) -> int {
    File file = LittleFS.open(LIVE_FILE, "r");
    if (!file) {
        return -1;
    }
    file.seek(sizeof(Header));
    Events::Record record;
    for (size_t slot = 0; slot < g_count && readRecord(file, record); slot++) {
        if (strcmp(record.id, eventId) == 0) {
            if (out != nullptr) {
                *out = record;
            }

            return static_cast<int>(slot);
        }
    }

    return -1;
}

auto EventStore::upsert(const Events::Record& record) -> Result {
    Events::Record merged = record;
    bool replaced = false;
    const Result result = rewrite(
        [&](size_t /*slot*/, Events::Record& existing) {
            if (strcmp(existing.id, merged.id) == 0) {
                merged.lastFired = existing.lastFired;  // re-sending an event must not make it fire again
                existing = merged;
                replaced = true;
            }

            return true;
        },
        nullptr);
    if (result != Result::Ok || replaced) {
        return result;
    }

    return rewrite([](size_t /*slot*/, Events::Record& /*existing*/) { return true; }, &merged);
}

auto EventStore::remove(const char* eventId) -> Result {
    bool found = false;
    const Result result = rewrite(
        [&](size_t /*slot*/, Events::Record& existing) {
            if (strcmp(existing.id, eventId) == 0) {
                found = true;

                return false;
            }

            return true;
        },
        nullptr);

    return (result == Result::Ok && !found) ? Result::NotFound : result;
}

auto EventStore::removeSlots(uint64_t slots) -> Result {
    if (slots == 0) {
        return Result::Ok;
    }

    return rewrite([slots](size_t slot, Events::Record& /*existing*/) { return (slots & (1ULL << slot)) == 0; }, nullptr);
}

auto EventStore::clear() -> Result {
    LittleFS.remove(LIVE_FILE);
    g_count = 0;
    g_revision++;

    return Result::Ok;
}

void EventStore::markFired(size_t slot, uint32_t when) {
    File file = LittleFS.open(LIVE_FILE, "r+");
    if (!file || !file.seek(recordOffset(slot) + offsetof(Events::Record, lastFired))) {
        return;
    }
    file.write(reinterpret_cast<const uint8_t*>(&when), sizeof(when));
    file.close();
    // no revision bump: nothing about the schedule changed, only the bookkeeping
}

auto EventStore::beginReplace() -> Result {
    abortReplace();
    File file = LittleFS.open(STAGING_FILE, "w");
    if (!file) {
        return Result::IoError;
    }
    const bool success = writeHeader(file, 0);
    file.close();
    g_staging = success;
    g_staged = 0;

    return success ? Result::Ok : Result::IoError;
}

auto EventStore::appendReplace(const Events::Record& record) -> Result {
    if (!g_staging) {
        return Result::NotFound;  // no replace in progress
    }
    if (g_staged >= Events::MAX_EVENTS) {
        return Result::Full;
    }
    Events::Record merged = record;
    Events::Record existing;
    if (find(merged.id, &existing) >= 0) {
        merged.lastFired = existing.lastFired;  // keep the "already fired" bookkeeping across a full replace
    }
    File file = LittleFS.open(STAGING_FILE, "a");
    if (!file) {
        return Result::IoError;
    }
    const bool success = writeRecord(file, merged);
    file.close();
    if (success) {
        g_staged++;
    }

    return success ? Result::Ok : Result::IoError;
}

auto EventStore::commitReplace() -> Result {
    if (!g_staging) {
        return Result::NotFound;
    }
    File file = LittleFS.open(STAGING_FILE, "r+");
    if (!file) {
        return Result::IoError;
    }
    const bool success = file.seek(0) && writeHeader(file, g_staged);
    file.close();
    g_staging = false;
    if (!success || !commitFile(STAGING_FILE, g_staged)) {
        LittleFS.remove(STAGING_FILE);

        return Result::IoError;
    }

    return Result::Ok;
}

void EventStore::abortReplace() {
    if (g_staging) {
        LittleFS.remove(STAGING_FILE);
    }
    g_staging = false;
    g_staged = 0;
}

auto EventStore::replaceCount() -> size_t { return g_staged; }
