// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// On-flash format of the usage cache: the last good Claude and Codex snapshots, so a reboot shows real (dimmed, if old)
// numbers instead of "Loading". Pure (no Arduino) so it is unit-tested on the host.
//
// Layout: header { magic, version, claudeSize, codexSize } followed by the two structs byte for byte. A cache written
// by a different build (struct sizes differ) or a damaged file is rejected and ignored, never trusted.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "usage_types.h"

namespace UsageCacheFormat {

constexpr uint32_t kMagic = 0x55434331;  // "UCC1"
constexpr uint16_t kVersion = 1;

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t claudeSize;
    uint16_t codexSize;
    uint16_t reserved;
};

constexpr size_t kEncodedSize = sizeof(Header) + sizeof(ClaudeData) + sizeof(CodexData);

// Returns the number of bytes written, 0 if `capacity` is too small.
inline size_t encode(const ClaudeData& claude, const CodexData& codex, uint8_t* out, size_t capacity) {
    if (capacity < kEncodedSize) return 0;
    Header header = {kMagic, kVersion, (uint16_t)sizeof(ClaudeData), (uint16_t)sizeof(CodexData), 0};
    memcpy(out, &header, sizeof(header));
    memcpy(out + sizeof(header), &claude, sizeof(ClaudeData));
    memcpy(out + sizeof(header) + sizeof(ClaudeData), &codex, sizeof(CodexData));
    return kEncodedSize;
}

// On success `claude` / `codex` are replaced; on failure they are left untouched. Run-time-only fields are reset: the
// credential state and the surfaced error are derived fresh, never restored.
inline bool decode(const uint8_t* in, size_t size, ClaudeData& claude, CodexData& codex) {
    if (size != kEncodedSize) return false;
    Header header;
    memcpy(&header, in, sizeof(header));
    if (header.magic != kMagic || header.version != kVersion || header.claudeSize != sizeof(ClaudeData) ||
        header.codexSize != sizeof(CodexData))
        return false;
    ClaudeData restoredClaude;
    CodexData restoredCodex;
    memcpy(&restoredClaude, in + sizeof(header), sizeof(ClaudeData));
    memcpy(&restoredCodex, in + sizeof(header) + sizeof(ClaudeData), sizeof(CodexData));
    restoredClaude.cred = Cred::NOT_SET;
    restoredClaude.err[0] = '\0';
    restoredCodex.cred = Cred::NOT_SET;
    restoredCodex.err[0] = '\0';
    // A snapshot with no reading in it is not worth showing
    if (!restoredClaude.valid) restoredClaude = ClaudeData();
    if (!restoredCodex.valid) restoredCodex = CodexData();
    claude = restoredClaude;
    codex = restoredCodex;
    return true;
}

}  // namespace UsageCacheFormat
