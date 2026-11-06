// Host tests for the event schedule arithmetic, run against real timezone rules. Build and run with scripts/host-tests.sh.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>

#include "events/schedule.h"

using namespace Schedule;

// UTC epoch of a UTC wall-clock moment (independent of the TZ under test)
static time_t utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
    tm t{};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    return timegm(&t);
}

static void localIs(time_t when, int h, int mi, int mday, int mon) {
    tm t{};
    localtime_r(&when, &t);
    if (t.tm_hour != h || t.tm_min != mi || t.tm_mday != mday || t.tm_mon != mon - 1) {
        printf("expected local %02d:%02d on %d/%d, got %02d:%02d on %d/%d\n", h, mi, mday, mon, t.tm_hour, t.tm_min, t.tm_mday, t.tm_mon + 1);
        assert(false);
    }
}

static Spec daily(int h, int m) { Spec s; s.repeat = Repeat::Daily; s.minuteOfDay = h * 60 + m; return s; }

static void testOneOff() {
    Spec s; s.at = utc(2026, 11, 7, 9);
    assert(nextAfter(s, utc(2026, 11, 7, 8)) == s.at);
    assert(nextAfter(s, s.at) == 0);                                   // strictly after
    assert(dueNow(s, s.at, 0, 300) == s.at);                           // exactly on time
    assert(dueNow(s, s.at + 299, 0, 300) == s.at);                     // inside the grace window
    assert(dueNow(s, s.at + 301, 0, 300) == 0);                        // missed beyond grace: skipped
    assert(dueNow(s, s.at - 1, 0, 300) == 0);                          // not yet
    assert(dueNow(s, s.at + 10, s.at, 300) == 0);                      // already fired
    assert(!expired(s, s.at + 300, 300) && expired(s, s.at + 301, 300));
}

static void testDailyDst() {
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1); tzset();
    Spec s = daily(9, 0);
    time_t a = nextAfter(s, utc(2026, 3, 27, 12));                       // 28 Mar 09:00 GMT
    time_t b = nextAfter(s, a);                                          // 29 Mar 09:00 BST (clocks went forward at 01:00)
    time_t c = nextAfter(s, b);                                          // 30 Mar 09:00 BST
    localIs(a, 9, 0, 28, 3); localIs(b, 9, 0, 29, 3); localIs(c, 9, 0, 30, 3);
    assert(b - a == 23 * 3600);                                          // the spring-forward day is 23 hours long
    assert(c - b == 24 * 3600);
    // autumn: 25 Oct is 25 hours long
    time_t d = nextAfter(s, utc(2026, 10, 24, 12));                      // 25 Oct 09:00 GMT
    time_t e = nextAfter(s, d);                                          // 26 Oct
    localIs(d, 9, 0, 25, 10); localIs(e, 9, 0, 26, 10);
    assert(e - d == 24 * 3600 && d - nextAfter(s, utc(2026, 10, 23, 12)) == 25 * 3600);
}

static void testDstEdgeTimes() {
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1); tzset();
    // 01:30 does not exist on 2026-03-29 in London: still exactly one occurrence that day, then the next day
    Spec gap = daily(1, 30);
    time_t before = utc(2026, 3, 28, 12);
    time_t n1 = nextAfter(gap, before), n2 = nextAfter(gap, n1), n3 = nextAfter(gap, n2);
    tm t{}; localtime_r(&n1, &t);
    assert(t.tm_mday == 29 && t.tm_mon == 2);                            // the gap day still has its occurrence (normalised to 02:30 BST)
    localtime_r(&n2, &t); assert(t.tm_mday == 30);                       // and exactly one: the next is the following day
    localtime_r(&n3, &t); assert(t.tm_mday == 31);
    assert(n1 < n2 && n2 < n3);
    // 01:30 happens twice on 2026-10-25: fires once
    Spec twice = daily(1, 30);
    time_t m1 = nextAfter(twice, utc(2026, 10, 24, 12)), m2 = nextAfter(twice, m1), m3 = nextAfter(twice, m2);
    localtime_r(&m1, &t); assert(t.tm_mday == 25);
    localtime_r(&m2, &t); assert(t.tm_mday == 26);
    localtime_r(&m3, &t); assert(t.tm_mday == 27);
}

static void testWeekly() {
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1); tzset();
    Spec s; s.repeat = Repeat::Weekly; s.minuteOfDay = 9 * 60 + 30;
    s.weekdayMask = (1 << 1) | (1 << 3);                                // Monday and Wednesday
    time_t friday = utc(2026, 11, 6, 12);                               // a Friday
    time_t t1 = nextAfter(s, friday); localIs(t1, 9, 30, 9, 11);        // Monday 9 Nov
    time_t t2 = nextAfter(s, t1);     localIs(t2, 9, 30, 11, 11);       // Wednesday 11 Nov
    time_t t3 = nextAfter(s, t2);     localIs(t3, 9, 30, 16, 11);       // Monday 16 Nov
    s.weekdayMask = 0;                                                  // no days selected: never fires
    assert(nextAfter(s, friday) == 0);
}

static void testMonthly() {
    setenv("TZ", "UTC0", 1); tzset();
    Spec s; s.repeat = Repeat::Monthly; s.minuteOfDay = 8 * 60; s.dayOfMonth = 31;
    time_t t = nextAfter(s, utc(2027, 1, 31, 9));                       // after Jan 31 08:00 -> last day of February (28, not a leap year)
    localIs(t, 8, 0, 28, 2);
    t = nextAfter(s, t); localIs(t, 8, 0, 31, 3);
    t = nextAfter(s, t); localIs(t, 8, 0, 30, 4);                       // April has 30 days
    Spec leap = s; time_t l = nextAfter(leap, utc(2028, 1, 31, 9)); localIs(l, 8, 0, 29, 2);   // 2028 is a leap year
    Spec first; first.repeat = Repeat::Monthly; first.minuteOfDay = 0; first.dayOfMonth = 1;
    t = nextAfter(first, utc(2026, 12, 31, 23, 59)); localIs(t, 0, 0, 1, 1);
}

static void testWindowAndGrace() {
    setenv("TZ", "UTC0", 1); tzset();
    Spec s = daily(9, 0);
    s.from = utc(2026, 11, 10);                                         // starts on the 10th
    s.until = utc(2026, 11, 12, 9);                                     // last occurrence: 12 Nov 09:00 inclusive
    time_t t = nextAfter(s, utc(2026, 11, 1)); localIs(t, 9, 0, 10, 11);
    t = nextAfter(s, t); localIs(t, 9, 0, 11, 11);
    t = nextAfter(s, t); localIs(t, 9, 0, 12, 11);
    assert(nextAfter(s, t) == 0);
    assert(expired(s, utc(2026, 11, 12, 9, 6), 300) && !expired(s, utc(2026, 11, 12, 9, 4), 300));
    // grace across local midnight: an event at 23:58 is still due at 00:02
    Spec late = daily(23, 58);
    time_t at = utc(2026, 11, 7, 23, 58);
    assert(dueNow(late, utc(2026, 11, 8, 0, 2), 0, 300) == at);
    assert(dueNow(late, utc(2026, 11, 8, 0, 2), at, 300) == 0);         // not twice
    assert(dueNow(late, utc(2026, 11, 8, 0, 4), 0, 300) == 0);          // 6 minutes late: skipped
    // after a reboot just inside the window it does not fire again
    Spec nine = daily(9, 0);
    time_t nineAt = utc(2026, 11, 7, 9);
    assert(dueNow(nine, nineAt + 60, 0, 300) == nineAt);
    assert(dueNow(nine, nineAt + 120, nineAt, 300) == 0);
}

static void testClockBeforeSync() {
    // a device that has just synced its clock: "now" jumps from 1970 to the real date. Nothing old may fire.
    setenv("TZ", "UTC0", 1); tzset();
    Spec s = daily(9, 0);
    assert(dueNow(s, 100, 0, 300) == 0);                                // 1970: no occurrence at 09:00 within grace
    Spec once; once.at = utc(2026, 1, 1);
    assert(dueNow(once, utc(2026, 11, 7), 0, 300) == 0);                // months old: skipped, and expired() lets it be deleted
    assert(expired(once, utc(2026, 11, 7), 300));
}

static void testPerformance() {
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1); tzset();
    Spec rare; rare.repeat = Repeat::Weekly; rare.minuteOfDay = 60; rare.weekdayMask = 1 << 6;   // Saturdays only
    time_t t = utc(2026, 1, 1);
    for (int i = 0; i < 400; i++) { time_t n = nextAfter(rare, t); assert(n > t); t = n; }          // 400 consecutive Saturdays
}

// A scheduler is a loop that ticks once a second. Simulate it over days, with lastFired kept the way the store keeps it,
// and count what fires. `rebootAt` loses all RAM state (not lastFired, which is on flash) at the given instants.
struct Sim {
    Spec spec;
    time_t lastFired = 0;
    bool deleted = false;
    int fires = 0;
    std::vector<time_t> firedAt;
};

static void tick(Sim& sim, time_t now) {
    if (sim.deleted) return;
    Verdict v = evaluate(sim.spec, now, sim.lastFired, DEFAULT_GRACE_S);
    if (v.fireAt != 0) {
        sim.fires++;
        sim.firedAt.push_back(v.fireAt);
        if (!v.remove) sim.lastFired = v.fireAt;
    }
    if (v.remove) sim.deleted = true;
}

static void testSimulation() {
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1); tzset();
    // daily 09:00 over 10 days that include the spring-forward day: exactly one firing per day, each at 09:00 local
    Sim daily9; daily9.spec = daily(9, 0);
    time_t start = utc(2026, 3, 25, 0), end = utc(2026, 4, 4, 0);
    for (time_t now = start; now < end; now += 1) tick(daily9, now);   // a tick every second, as on the device
    assert(daily9.fires == 10);
    for (time_t when : daily9.firedAt) { tm t{}; localtime_r(&when, &t); assert(t.tm_hour == 9 && t.tm_min == 0); }

    // and across the autumn change
    Sim autumn; autumn.spec = daily(9, 0);
    for (time_t now = utc(2026, 10, 22, 0); now < utc(2026, 10, 29, 0); now += 7) tick(autumn, now);   // a coarser tick still fires once
    assert(autumn.fires == 7);

    // a one-off fires once and is then deleted; it never fires again
    Sim once; once.spec.at = utc(2026, 11, 7, 9);
    for (time_t now = utc(2026, 11, 7, 8, 59); now < utc(2026, 11, 7, 10); now += 1) tick(once, now);
    assert(once.fires == 1 && once.deleted);

    // a ten-second stall (a blocking fetch) around the event time does not lose it
    Sim stall; stall.spec.at = utc(2026, 11, 7, 9);
    tick(stall, utc(2026, 11, 7, 8, 59, 55));
    tick(stall, utc(2026, 11, 7, 9, 0, 9));                              // the next tick is 14 s later
    assert(stall.fires == 1);

    // reboot: the process restarts at 09:02 (lastFired is on flash); it neither loses nor repeats the 09:00 event
    Sim reboot; reboot.spec = daily(9, 0);
    time_t t0 = utc(2026, 11, 7, 8, 59);
    for (time_t now = t0; now < utc(2026, 11, 7, 9, 1); now += 1) tick(reboot, now);
    assert(reboot.fires == 1);
    for (time_t now = utc(2026, 11, 7, 9, 2); now < utc(2026, 11, 7, 9, 30); now += 1) tick(reboot, now);   // "after the reboot"
    assert(reboot.fires == 1);

    // booted at 09:03 with no history (first boot after power loss): the missed 09:00 event is caught up once
    Sim late; late.spec = daily(9, 0);
    for (time_t now = utc(2026, 11, 7, 9, 3); now < utc(2026, 11, 7, 9, 30); now += 1) tick(late, now);
    assert(late.fires == 1);
    // booted at 09:10: too late, skipped; tomorrow's fires normally
    Sim tooLate; tooLate.spec = daily(9, 0);
    for (time_t now = utc(2026, 11, 7, 9, 10); now < utc(2026, 11, 8, 10); now += 1) tick(tooLate, now);
    assert(tooLate.fires == 1);
    tm t{}; localtime_r(&tooLate.firedAt[0], &t); assert(t.tm_mday == 8);

    // the clock jumping from 1970 to now (first NTP sync) must not release a flood of old one-offs
    Sim old; old.spec.at = utc(2026, 1, 1);
    tick(old, utc(2026, 11, 7, 9));
    assert(old.fires == 0 && old.deleted);                               // dropped, not fired

    // a weekly event, a window, and a monthly one: counts over a quarter
    Sim weekly; weekly.spec.repeat = Repeat::Weekly; weekly.spec.minuteOfDay = 9 * 60; weekly.spec.weekdayMask = (1 << 1) | (1 << 5);
    for (time_t now = utc(2026, 1, 1); now < utc(2026, 4, 1); now += 30) tick(weekly, now);
    assert(weekly.fires >= 25 && weekly.fires <= 27);                    // Mondays + Fridays in 13 weeks
    Sim monthly; monthly.spec.repeat = Repeat::Monthly; monthly.spec.dayOfMonth = 31; monthly.spec.minuteOfDay = 60;
    for (time_t now = utc(2026, 1, 1); now < utc(2027, 1, 1); now += 30) tick(monthly, now);
    assert(monthly.fires == 12);                                         // every month, clamped to its last day
    // a disabled-looking window that has ended: removed
    Sim ended; ended.spec = daily(9, 0); ended.spec.until = utc(2026, 11, 7, 9);
    for (time_t now = utc(2026, 11, 6); now < utc(2026, 11, 9); now += 30) tick(ended, now);
    assert(ended.fires == 2 && ended.deleted);                           // 6th and 7th, then gone
}

int main() {
    testSimulation();
    testOneOff();
    testDailyDst();
    testDstEdgeTimes();
    testWeekly();
    testMonthly();
    testWindowAndGrace();
    testClockBeforeSync();
    testPerformance();
    printf("schedule ok\n");
    return 0;
}
