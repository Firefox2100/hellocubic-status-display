// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include <string>

#include "ui/presence_mode.h"

int main() {
    using namespace Presence;
    Mode mode = Mode::Normal;
    for (const char* name : {"normal", "busy", "away", "relaxing"}) {
        assert(modeFrom(name, mode));
        assert(std::string(modeName(mode)) == name);
    }
    mode = Mode::Away;
    assert(!modeFrom("Busy", mode) && mode == Mode::Away);  // exact, case-sensitive; untouched on failure
    assert(!modeFrom("", mode) && !modeFrom(nullptr, mode) && !modeFrom("dnd", mode));

    assert(validGifName("arch-i-use-arch-btw.gif"));
    assert(!validGifName("") && !validGifName(nullptr) && !validGifName(".hidden"));
    assert(!validGifName("../x.gif") && !validGifName("a/b.gif") && !validGifName("a\\b.gif"));
    assert(!validGifName("tab\there.gif"));
    assert(!validGifName(std::string(GIF_MAX + 1, 'a').c_str()));
    assert(validGifName(std::string(GIF_MAX, 'a').c_str()));
    std::puts("presence ok");
}
