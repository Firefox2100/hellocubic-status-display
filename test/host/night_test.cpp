// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <cstdio>
#include <string>

#include "ui/night_core.h"

using namespace NightCore;

static void testParse() {
    int minutes = -1;
    assert(parseHm("00:00", minutes) && minutes == 0);
    assert(parseHm("22:30", minutes) && minutes == 1350);
    assert(parseHm("23:59", minutes) && minutes == 1439);
    for (const char* bad : {"24:00", "12:60", "7:30", "07:3", "0730", "ab:cd", "07-30", "", "07:300"}) {
        assert(!parseHm(bad, minutes));
    }
    assert(!parseHm(nullptr, minutes));
    char text[HM_SIZE];
    formatHm(1350, text, sizeof(text));
    assert(std::string(text) == "22:30");
    formatHm(5, text, sizeof(text));
    assert(std::string(text) == "00:05");
}

static void testNames() {
    Mode mode = Mode::Disabled;
    for (const char* name : {"disabled", "sunset", "schedule", "manual"}) {
        assert(modeFrom(name, mode) && std::string(modeName(mode)) == name);
    }
    assert(!modeFrom("Sunset", mode) && !modeFrom("off", mode) && !modeFrom(nullptr, mode));
    Style style = Style::Face;
    assert(styleFrom("off", style) && style == Style::Off);
    assert(styleFrom("face", style) && style == Style::Face);
    assert(!styleFrom("dark", style));
}

static void testWindow() {
    assert(inWindow(23 * 60, 22 * 60, 7 * 60));      // wraps midnight
    assert(inWindow(60, 22 * 60, 7 * 60));
    assert(!inWindow(12 * 60, 22 * 60, 7 * 60));
    assert(inWindow(22 * 60, 22 * 60, 7 * 60));      // start inclusive
    assert(!inWindow(7 * 60, 22 * 60, 7 * 60));      // end exclusive
    assert(inWindow(13 * 60, 12 * 60, 14 * 60));     // same-day window
    assert(!inWindow(15 * 60, 12 * 60, 14 * 60));
    assert(!inWindow(0, 600, 600));                  // empty
}

static void testSun() {
    // three days: sunrise 07:00, sunset 17:00 (in seconds from an arbitrary midnight)
    const time_t day = 86400;
    const time_t base = 1000000000;
    const time_t rise[3] = {base + 7 * 3600, base + day + 7 * 3600, base + 2 * day + 7 * 3600};
    const time_t set[3] = {base + 17 * 3600, base + day + 17 * 3600, base + 2 * day + 17 * 3600};
    assert(bySun(base + 3600, rise, set) == 1);                    // before the first sunrise
    assert(bySun(base + 12 * 3600, rise, set) == 0);               // daytime
    assert(bySun(base + 18 * 3600, rise, set) == 1);               // after sunset
    assert(bySun(base + day + 8 * 3600, rise, set) == 0);          // tomorrow morning
    assert(bySun(base + 2 * day + 18 * 3600, rise, set) == -1);    // after the data ends
    assert(bySun(base + 5 * day, rise, set) == -1);
    const time_t none[3] = {0, 0, 0};
    assert(bySun(base, none, none) == -1);
    // stale data from yesterday still answers correctly when it covers now
    const time_t old_rise[3] = {rise[0] - day, rise[0], rise[1]};
    const time_t old_set[3] = {set[0] - day, set[0], set[1]};
    assert(bySun(base + 12 * 3600, old_rise, old_set) == 0);
}

static void testOverride() {
    Override over;
    assert(!resolve(false, over) && resolve(true, over));          // no override: follows automatic
    setOverride(over, true, false);                                 // day automatically, switched to night
    assert(resolve(false, over) && resolve(false, over));           // stays night
    assert(resolve(true, over) && over.want == -1);                 // automatic reaches night: override ends
    assert(resolve(true, over));
    setOverride(over, false, true);                                 // night automatically, switched to day
    assert(!resolve(true, over));
    assert(!resolve(false, over) && over.want == -1);               // automatic reaches day: override ends
    setOverride(over, true, false);
    setOverride(over, false, false);                                // back to what automatic says: cleared
    assert(over.want == -1 && !resolve(false, over));
}

int main() {
    testParse();
    testNames();
    testWindow();
    testSun();
    testOverride();
    std::puts("night_test: ok");
    return 0;
}
