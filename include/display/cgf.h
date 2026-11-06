// SPDX-License-Identifier: GPL-3.0-or-later
// CGF1: a small animation format the cube can stream to the panel with ~1.5 KB of RAM (a GIF decoder needs 24 KB).
// Produced on a PC by tools/gif2cgf.py. Pure logic (no Arduino types), host-tested by test/host/cgf_test.cpp.
//
// File: header, palette, frames. All integers little-endian.
//   header  12 B: "CGF1", u16 width, u16 height, u16 frames, u8 colors (1..255), u8 reserved
//   palette colors * u16 RGB565 (palette index 255 is never a colour: it means "unchanged")
//   frame   14 B: u16 duration_ms, u16 x, u16 y, u16 w, u16 h, u32 dataLen, then dataLen bytes
//           w == 0: nothing changed. Otherwise w*h palette indices, row by row, packed as
//           control byte c: c < 0x80 -> (c+1) literal bytes follow; c >= 0x80 -> next byte repeated (c&0x7F)+1 times.
//           Index 255 leaves the screen pixel as it is (frames after the first only send what changed).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Cgf {

constexpr uint8_t UNCHANGED = 255;
constexpr uint16_t MAX_DIM = 240;
constexpr size_t HEADER_BYTES = 12;
constexpr size_t FRAME_HEADER_BYTES = 14;
constexpr size_t MAX_COLORS = 255;
constexpr size_t READ_CHUNK = 128;

/** @brief Where the bytes come from (a LittleFS file on the device, a stdio file in tests). */
class Source {
   public:
    virtual ~Source() = default;
    virtual auto read(uint8_t* dst, size_t len) -> size_t = 0;
    virtual auto seek(uint32_t pos) -> bool = 0;
};

/** @brief Where the pixels go: a run of `len` RGB565 pixels at (x, y) in image coordinates. */
class Sink {
   public:
    virtual ~Sink() = default;
    virtual void span(int x, int y, int len, const uint16_t* pixels) = 0;
};

class Player {
   public:
    /** @brief Read the header and palette. @return false if the file is not a usable CGF1. */
    auto open(Source& src) -> bool {
        m_src = &src;
        uint8_t head[HEADER_BYTES];
        if (!src.seek(0) || src.read(head, HEADER_BYTES) != HEADER_BYTES || memcmp(head, "CGF1", 4) != 0) {
            return false;
        }
        m_width = le16(head + 4);
        m_height = le16(head + 6);
        m_frames = le16(head + 8);
        m_colors = head[10];
        if (m_width == 0 || m_height == 0 || m_width > MAX_DIM || m_height > MAX_DIM || m_frames == 0 || m_colors == 0) {
            return false;
        }
        for (size_t i = 0; i < m_colors; i++) {
            uint8_t pair[2];
            if (src.read(pair, 2) != 2) {
                return false;
            }
            m_palette[i] = le16(pair);
        }
        m_framesStart = static_cast<uint32_t>(HEADER_BYTES + (static_cast<size_t>(m_colors) * 2));
        m_next = 0;

        return src.seek(m_framesStart);
    }

    auto width() const -> int { return m_width; }
    auto height() const -> int { return m_height; }
    auto frameCount() const -> int { return m_frames; }

    /**
     * @brief Decode the next frame into `sink`, wrapping to the first frame after the last.
     * @return the frame's duration in ms (>= 1), or 0 when the file is damaged (the caller should stop)
     */
    auto nextFrame(Sink& sink) -> uint16_t {
        if (m_next >= m_frames) {
            if (!m_src->seek(m_framesStart)) {
                return 0;
            }
            m_next = 0;
        }
        m_next++;

        uint8_t head[FRAME_HEADER_BYTES];
        if (m_src->read(head, FRAME_HEADER_BYTES) != FRAME_HEADER_BYTES) {
            return 0;
        }
        const uint16_t duration = le16(head);
        const int x0 = le16(head + 2);
        const int y0 = le16(head + 4);
        const int w = le16(head + 6);
        const int h = le16(head + 8);
        const uint32_t len = le32(head + 10);
        if (w == 0) {
            return skip(len) ? clampDuration(duration) : 0;
        }
        if (x0 + w > m_width || y0 + h > m_height || w > MAX_DIM) {
            return 0;
        }

        m_bufPos = 0;
        m_bufLen = 0;
        m_remaining = len;
        m_runLeft = 0;
        m_litLeft = 0;
        for (int row = 0; row < h; row++) {
            if (!decodeRow(w)) {
                return 0;
            }
            emitRow(sink, x0, y0 + row, w);
        }
        // A well-formed frame consumes exactly dataLen bytes; skip any padding to stay in step
        return skip(m_remaining + (m_bufLen - m_bufPos)) ? clampDuration(duration) : 0;
    }

   private:
    static auto le16(const uint8_t* ptr) -> uint16_t { return static_cast<uint16_t>(ptr[0] | (ptr[1] << 8)); }
    static auto le32(const uint8_t* ptr) -> uint32_t {
        return static_cast<uint32_t>(ptr[0]) | (static_cast<uint32_t>(ptr[1]) << 8) |
               (static_cast<uint32_t>(ptr[2]) << 16) | (static_cast<uint32_t>(ptr[3]) << 24);
    }
    static auto clampDuration(uint16_t ms) -> uint16_t { return ms == 0 ? 1 : ms; }

    /** @brief Next byte of the current frame's data, refilling the small read buffer. */
    auto nextByte(uint8_t& out) -> bool {
        if (m_bufPos >= m_bufLen) {
            if (m_remaining == 0) {
                return false;
            }
            const size_t want = m_remaining < READ_CHUNK ? m_remaining : READ_CHUNK;
            m_bufLen = m_src->read(m_buf, want);
            m_bufPos = 0;
            if (m_bufLen == 0) {
                return false;
            }
            m_remaining -= static_cast<uint32_t>(m_bufLen);
        }
        out = m_buf[m_bufPos++];

        return true;
    }

    auto skip(uint32_t count) -> bool {
        // consume what is buffered first, then jump over the rest of the data
        const uint32_t buffered = static_cast<uint32_t>(m_bufLen - m_bufPos);
        m_bufPos = m_bufLen = 0;
        if (count <= buffered) {
            return true;
        }
        const uint32_t rest = count - buffered;
        // Source has no relative seek: read forward and drop
        uint8_t scratch[READ_CHUNK];
        uint32_t left = rest;
        while (left > 0) {
            const size_t step = left < READ_CHUNK ? left : READ_CHUNK;
            if (m_src->read(scratch, step) != step) {
                return false;
            }
            left -= static_cast<uint32_t>(step);
        }

        return true;
    }

    auto decodeRow(int width) -> bool {
        for (int i = 0; i < width;) {
            if (m_runLeft > 0) {
                m_row[i++] = m_runValue;
                m_runLeft--;
            } else if (m_litLeft > 0) {
                uint8_t val = 0;
                if (!nextByte(val)) {
                    return false;
                }
                m_row[i++] = val;
                m_litLeft--;
            } else {
                uint8_t ctl = 0;
                if (!nextByte(ctl)) {
                    return false;
                }
                if ((ctl & 0x80U) != 0) {
                    if (!nextByte(m_runValue)) {
                        return false;
                    }
                    m_runLeft = static_cast<uint8_t>((ctl & 0x7FU) + 1U);
                } else {
                    m_litLeft = static_cast<uint8_t>(ctl + 1U);
                }
            }
        }

        return true;
    }

    void emitRow(Sink& sink, int x0, int y, int width) {
        int i = 0;
        while (i < width) {
            while (i < width && m_row[i] == UNCHANGED) {
                i++;
            }
            const int start = i;
            while (i < width && m_row[i] != UNCHANGED) {
                m_line[i - start] = m_row[i] < m_colors ? m_palette[m_row[i]] : 0;
                i++;
            }
            if (i > start) {
                sink.span(x0 + start, y, i - start, m_line);
            }
        }
    }

    Source* m_src = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_frames = 0;
    int m_next = 0;
    uint8_t m_colors = 0;
    uint32_t m_framesStart = 0;

    uint16_t m_palette[MAX_COLORS] = {};
    uint8_t m_row[MAX_DIM] = {};
    uint16_t m_line[MAX_DIM] = {};
    uint8_t m_buf[READ_CHUNK] = {};
    size_t m_bufPos = 0;
    size_t m_bufLen = 0;
    uint32_t m_remaining = 0;
    uint8_t m_runValue = 0;
    uint8_t m_runLeft = 0;
    uint8_t m_litLeft = 0;
};

}  // namespace Cgf
