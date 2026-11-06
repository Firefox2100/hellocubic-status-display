// SPDX-License-Identifier: GPL-3.0-or-later
// Plays test/host/fixtures/sample.cgf (made by tools/gif2cgf.py) and checks every frame against the converter's own
// expected canvas, then feeds the decoder damaged and random data.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "display/cgf.h"

namespace {
class MemSource : public Cgf::Source {
   public:
    explicit MemSource(std::vector<uint8_t> data) : m_data(std::move(data)) {}
    auto read(uint8_t* dst, size_t len) -> size_t override {
        const size_t n = m_pos + len <= m_data.size() ? len : m_data.size() - m_pos;
        memcpy(dst, m_data.data() + m_pos, n);
        m_pos += n;
        return n;
    }
    auto seek(uint32_t pos) -> bool override {
        if (pos > m_data.size()) return false;
        m_pos = pos;
        return true;
    }

   private:
    std::vector<uint8_t> m_data;
    size_t m_pos = 0;
};

class CanvasSink : public Cgf::Sink {
   public:
    CanvasSink(int w, int h) : w(w), h(h), px(static_cast<size_t>(w * h), 0) {}
    void span(int x, int y, int len, const uint16_t* pixels) override {
        assert(x >= 0 && y >= 0 && y < h && len > 0 && x + len <= w);
        for (int i = 0; i < len; i++) px[static_cast<size_t>(y * w + x + i)] = pixels[i];
        spans++;
    }
    int w, h, spans = 0;
    std::vector<uint16_t> px;
};

auto slurp(const char* path) -> std::vector<uint8_t> {
    FILE* f = fopen(path, "rb");
    assert(f && "run from the repo root");
    std::vector<uint8_t> out;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}
}  // namespace

int main() {
    const auto cgf = slurp("test/host/fixtures/sample.cgf");
    const auto raw = slurp("test/host/fixtures/sample.raw");

    MemSource src(cgf);
    Cgf::Player player;
    assert(player.open(src));
    const int w = player.width(), h = player.height(), frames = player.frameCount();
    assert(w == 41 && h == 25 && frames == 8);
    assert(raw.size() == static_cast<size_t>(w * h * frames * 2));

    // Two full loops: the second proves the wrap back to frame 0 restores everything.
    CanvasSink sink(w, h);
    const uint16_t expectMs[8] = {40, 40, 50, 120, 40, 40, 40, 90};  // Pillow merged the identical frame 4 into 3
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < frames; k++) {
            const int before = sink.spans;
            const uint16_t ms = player.nextFrame(sink);
            assert(ms == expectMs[k]);
            (void)before;
            const uint8_t* want = raw.data() + static_cast<size_t>(k * w * h * 2);
            for (int i = 0; i < w * h; i++) {
                const uint16_t exp = static_cast<uint16_t>(want[i * 2] | (want[i * 2 + 1] << 8));
                if (sink.px[static_cast<size_t>(i)] != exp) {
                    std::printf("mismatch pass %d frame %d pixel %d: got %04x want %04x\n", pass, k, i,
                                sink.px[static_cast<size_t>(i)], exp);
                    return 1;
                }
            }
        }
    }

    // A frame with nothing changed (w == 0) draws nothing and still lasts its duration; handmade stream.
    {
        // 2x1 image, 2 frames, 1 colour (0x1234). Frame 0: dur 10, full, data = run of 2 x index 0. Frame 1: dur 25, w = 0.
        std::vector<uint8_t> tiny;
        tiny.assign({'C', 'G', 'F', '1', 2, 0, 1, 0, 2, 0, 1, 0, 0x34, 0x12,
                     10, 0, 0, 0, 0, 0, 2, 0, 1, 0, 2, 0, 0, 0, 0x81, 0x00,
                     25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        MemSource tsrc(tiny);
        Cgf::Player tp;
        assert(tp.open(tsrc));
        CanvasSink ts(2, 1);
        assert(tp.nextFrame(ts) == 10 && ts.px[0] == 0x1234 && ts.px[1] == 0x1234 && ts.spans == 1);
        assert(tp.nextFrame(ts) == 25 && ts.spans == 1);  // nothing drawn
        assert(tp.nextFrame(ts) == 10 && ts.spans == 2);  // wrapped to frame 0
    }

    // Damaged input must be refused or stop with 0, never crash or draw outside the image.
    for (size_t cut : {size_t(0), size_t(5), size_t(11), size_t(40), cgf.size() / 2, cgf.size() - 1}) {
        MemSource trunc(std::vector<uint8_t>(cgf.begin(), cgf.begin() + static_cast<long>(cut)));
        Cgf::Player p;
        CanvasSink s(w, h);
        if (p.open(trunc)) {
            for (int k = 0; k < frames + 2; k++) {
                if (p.nextFrame(s) == 0) break;
            }
        }
    }
    srand(1);
    for (int round = 0; round < 3000; round++) {
        std::vector<uint8_t> bad = cgf;
        const int flips = 1 + rand() % 6;
        for (int f = 0; f < flips; f++) bad[static_cast<size_t>(rand()) % bad.size()] = static_cast<uint8_t>(rand());
        MemSource msrc(bad);
        Cgf::Player p;
        CanvasSink s(240, 240);  // big enough that a bad header cannot trip the sink assertion for the wrong reason
        if (!p.open(msrc)) continue;
        CanvasSink bounded(p.width(), p.height());
        for (int k = 0; k < frames + 2; k++) {
            if (p.nextFrame(bounded) == 0) break;
        }
    }
    std::puts("cgf ok");
}
