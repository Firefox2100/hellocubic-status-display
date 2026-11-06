// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

#include "events/event_record.h"

/**
 * @brief The schedule on flash: /events.bin, a header plus up to MAX_EVENTS fixed-size records.
 *
 * Operations stream the file record by record through a temp file and rename it over the original, so a power cut
 * leaves either the old or the new schedule, never a half-written one, and no more than one record is ever held in
 * RAM. A file that does not match the expected layout is moved aside and the schedule starts empty.
 */
namespace EventStore {

enum class Result : uint8_t { Ok, Full, IoError, NotFound };

/** @brief Check the file; call once after the filesystem is mounted. */
void begin();

auto count() -> size_t;

/** @brief Incremented on every change, so the scheduler knows to look again. */
auto revision() -> uint32_t;

auto read(size_t slot, Events::Record& out) -> bool;

/** @brief The slot of the event with this id, or -1. Fills `out` when given. */
auto find(const char* eventId, Events::Record* out = nullptr) -> int;

/** @brief Insert, or replace the event with the same id. A known id keeps its `lastFired`. */
auto upsert(const Events::Record& record) -> Result;

auto remove(const char* eventId) -> Result;

/** @brief Remove the events whose slot bit is set in `slots` (bit i = slot i), in one rewrite. */
auto removeSlots(uint64_t slots) -> Result;

auto clear() -> Result;

/** @brief Record that the occurrence at `when` fired, in place (a small write). */
void markFired(size_t slot, uint32_t when);

// Replace the whole schedule in several requests: begin, append any number of times, then commit (atomic swap).
auto beginReplace() -> Result;
auto appendReplace(const Events::Record& record) -> Result;
auto commitReplace() -> Result;
void abortReplace();
auto replaceCount() -> size_t;

}  // namespace EventStore
