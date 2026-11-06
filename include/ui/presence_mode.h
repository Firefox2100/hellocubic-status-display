// SPDX-License-Identifier: GPL-3.0-or-later
// Pure logic of the "presence" state (busy / away / relaxing), host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace Presence {

/** @brief What the cube says about its owner when no card is holding the screen. */
enum class Mode : uint8_t {
    Normal,    ///< clock + usage screens + agents list (the rotation)
    Busy,      ///< red "busy" screen: do not disturb
    Away,      ///< "away": not coming back soon
    Relaxing,  ///< the owner's GIF
};

constexpr size_t TEXT_MAX = 27;  ///< optional line under the big word ("back at 14:00")
constexpr size_t GIF_MAX = 31;   ///< GIF file name inside /gif

inline auto modeName(Mode mode) -> const char* {
    switch (mode) {
        case Mode::Busy:
            return "busy";
        case Mode::Away:
            return "away";
        case Mode::Relaxing:
            return "relaxing";
        case Mode::Normal:
            break;
    }

    return "normal";
}

/** @brief Parse a mode name (exact, lower case). @return false if unknown (`out` untouched). */
inline auto modeFrom(const char* name, Mode& out) -> bool {
    if (name == nullptr) {
        return false;
    }
    for (const Mode mode : {Mode::Normal, Mode::Busy, Mode::Away, Mode::Relaxing}) {
        if (strcmp(name, modeName(mode)) == 0) {
            out = mode;

            return true;
        }
    }

    return false;
}

/** @brief A GIF name is a plain file name: no path parts, no control characters. */
inline auto validGifName(const char* name) -> bool {
    if (name == nullptr || name[0] == '\0' || strlen(name) > GIF_MAX || name[0] == '.') {
        return false;
    }
    for (const char* cur = name; *cur != '\0'; cur++) {
        if (*cur == '/' || *cur == '\\' || static_cast<unsigned char>(*cur) < ' ') {
            return false;
        }
    }

    return true;
}

}  // namespace Presence
