// Host tests for src/events/EventStore.cpp, run against a stdio-backed LittleFS stand-in (test/host/stubs).
// Build and run with scripts/host-tests.sh.
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <LittleFS.h>

#include "events/EventStore.h"

using namespace Events;

static std::string dir;

static void freshFs() {
    std::string command = "rm -rf " + dir + " && mkdir -p " + dir;
    assert(system(command.c_str()) == 0);
    LittleFSStub::root = dir + "/";
    EventStore::begin();
}

static Record make(const char* id, uint32_t at = 1800000000, const char* title = "t") {
    Record record = blank();
    strncpy(record.id, id, ID_LEN);
    strncpy(record.title, title, TITLE_LEN);
    record.at = at;
    return record;
}

static std::string ids() {
    std::string out;
    Record record;
    for (size_t i = 0; i < EventStore::count(); i++) {
        assert(EventStore::read(i, record));
        out += record.id;
        out += ",";
    }
    return out;
}

static void testBasics() {
    freshFs();
    assert(EventStore::count() == 0);
    assert(EventStore::find("a") < 0);
    uint32_t rev = EventStore::revision();
    assert(EventStore::upsert(make("a", 100)) == EventStore::Result::Ok);
    assert(EventStore::upsert(make("b", 200)) == EventStore::Result::Ok);
    assert(EventStore::upsert(make("c", 300)) == EventStore::Result::Ok);
    assert(EventStore::count() == 3 && ids() == "a,b,c,");
    assert(EventStore::revision() > rev);
    Record record;
    assert(EventStore::find("b", &record) == 1 && record.at == 200);
    assert(EventStore::find("zzz") < 0);

    // same id replaces in place (order kept, count unchanged)
    assert(EventStore::upsert(make("b", 222, "renamed")) == EventStore::Result::Ok);
    assert(EventStore::count() == 3 && ids() == "a,b,c,");
    assert(EventStore::find("b", &record) == 1 && record.at == 222 && !strcmp(record.title, "renamed"));
}

static void testLastFiredIsKept() {
    freshFs();
    EventStore::upsert(make("daily"));
    EventStore::markFired(0, 1790000000);
    Record record;
    assert(EventStore::find("daily", &record) == 0 && record.lastFired == 1790000000u);
    uint32_t rev = EventStore::revision();
    EventStore::markFired(0, 1790086400);                                // bookkeeping only: no schedule change
    assert(EventStore::revision() == rev);
    // re-sending the event (as a sync script does) must not forget that it already fired
    assert(EventStore::upsert(make("daily", 5, "again")) == EventStore::Result::Ok);
    assert(EventStore::find("daily", &record) == 0 && record.lastFired == 1790086400u && !strcmp(record.title, "again"));
}

static void testRemove() {
    freshFs();
    for (const char* id : {"a", "b", "c", "d"}) EventStore::upsert(make(id));
    assert(EventStore::remove("b") == EventStore::Result::Ok && ids() == "a,c,d,");
    assert(EventStore::remove("b") == EventStore::Result::NotFound && ids() == "a,c,d,");
    assert(EventStore::removeSlots((1ULL << 0) | (1ULL << 2)) == EventStore::Result::Ok && ids() == "c,");
    assert(EventStore::removeSlots(0) == EventStore::Result::Ok && ids() == "c,");
    assert(EventStore::clear() == EventStore::Result::Ok && EventStore::count() == 0);
    assert(EventStore::upsert(make("again")) == EventStore::Result::Ok && ids() == "again,");   // usable after clear
}

static void testCapacity() {
    freshFs();
    for (size_t i = 0; i < MAX_EVENTS; i++) {
        char id[ID_LEN + 1];
        snprintf(id, sizeof(id), "e%02zu", i);
        assert(EventStore::upsert(make(id, 1000 + i)) == EventStore::Result::Ok);
    }
    assert(EventStore::count() == MAX_EVENTS);
    assert(EventStore::upsert(make("overflow")) == EventStore::Result::Full);
    assert(EventStore::count() == MAX_EVENTS && EventStore::find("overflow") < 0);
    // a full schedule can still be edited: replacing an existing id is not an insert
    assert(EventStore::upsert(make("e10", 9999)) == EventStore::Result::Ok);
    Record record;
    assert(EventStore::find("e10", &record) == 10 && record.at == 9999u && EventStore::count() == MAX_EVENTS);
    // ...and deleting makes room
    assert(EventStore::remove("e00") == EventStore::Result::Ok);
    assert(EventStore::upsert(make("overflow")) == EventStore::Result::Ok && EventStore::count() == MAX_EVENTS);
}

static void testPersistence() {
    freshFs();
    EventStore::upsert(make("a", 11));
    EventStore::upsert(make("b", 22));
    EventStore::markFired(1, 777);
    EventStore::begin();                                                 // "reboot": state is re-read from the file
    assert(EventStore::count() == 2 && ids() == "a,b,");
    Record record;
    assert(EventStore::find("b", &record) == 1 && record.at == 22u && record.lastFired == 777u);
}

static void testStagedReplace() {
    freshFs();
    EventStore::upsert(make("keep", 1));
    EventStore::upsert(make("drop", 2));
    EventStore::markFired(0, 555);                                       // "keep" has fired before
    // nothing staged yet: append / commit are refused and the live file is untouched
    assert(EventStore::appendReplace(make("x")) == EventStore::Result::NotFound);
    assert(EventStore::commitReplace() == EventStore::Result::NotFound);

    assert(EventStore::beginReplace() == EventStore::Result::Ok);
    assert(EventStore::appendReplace(make("new1", 10)) == EventStore::Result::Ok);
    assert(EventStore::appendReplace(make("keep", 99)) == EventStore::Result::Ok);
    assert(EventStore::replaceCount() == 2);
    assert(ids() == "keep,drop,");                                       // the live schedule is unchanged until commit
    assert(EventStore::appendReplace(make("new2", 20)) == EventStore::Result::Ok);
    assert(EventStore::commitReplace() == EventStore::Result::Ok);
    assert(ids() == "new1,keep,new2,");
    Record record;
    assert(EventStore::find("keep", &record) == 1 && record.at == 99u && record.lastFired == 555u);   // bookkeeping survived
    assert(EventStore::find("drop") < 0);
    EventStore::begin();
    assert(ids() == "new1,keep,new2,");                                  // and it is what is on flash

    // abort leaves the live schedule alone
    assert(EventStore::beginReplace() == EventStore::Result::Ok);
    EventStore::appendReplace(make("zzz"));
    EventStore::abortReplace();
    assert(EventStore::commitReplace() == EventStore::Result::NotFound);
    assert(ids() == "new1,keep,new2,");

    // an empty replacement clears the schedule
    assert(EventStore::beginReplace() == EventStore::Result::Ok);
    assert(EventStore::commitReplace() == EventStore::Result::Ok && EventStore::count() == 0);

    // more than the capacity: refused, old schedule kept
    EventStore::upsert(make("old"));
    assert(EventStore::beginReplace() == EventStore::Result::Ok);
    for (size_t i = 0; i < MAX_EVENTS; i++) {
        char id[ID_LEN + 1];
        snprintf(id, sizeof(id), "n%02zu", i);
        assert(EventStore::appendReplace(make(id)) == EventStore::Result::Ok);
    }
    assert(EventStore::appendReplace(make("one-too-many")) == EventStore::Result::Full);
    EventStore::abortReplace();
    assert(ids() == "old,");
}

static void testDamagedFiles() {
    // garbage
    freshFs();
    FILE* f = fopen((dir + "/events.bin").c_str(), "wb");
    fputs("this is not an event file", f);
    fclose(f);
    EventStore::begin();
    assert(EventStore::count() == 0);
    assert(access((dir + "/events.bad").c_str(), F_OK) == 0);            // moved aside, not silently deleted
    assert(EventStore::upsert(make("fresh")) == EventStore::Result::Ok && ids() == "fresh,");

    // a truncated file (the header promises more records than are there)
    freshFs();
    EventStore::upsert(make("a"));
    EventStore::upsert(make("b"));
    std::string path = dir + "/events.bin";
    assert(truncate(path.c_str(), sizeof(Record) + 12 + 5) == 0);
    EventStore::begin();
    assert(EventStore::count() == 0);

    // an empty file
    freshFs();
    f = fopen((dir + "/events.bin").c_str(), "wb");
    fclose(f);
    EventStore::begin();
    assert(EventStore::count() == 0);

    // leftovers of an interrupted write are cleaned up at start
    freshFs();
    EventStore::upsert(make("a"));
    f = fopen((dir + "/events.tmp").c_str(), "wb"); fputs("junk", f); fclose(f);
    f = fopen((dir + "/events.new").c_str(), "wb"); fputs("junk", f); fclose(f);
    EventStore::begin();
    assert(access((dir + "/events.tmp").c_str(), F_OK) != 0 && access((dir + "/events.new").c_str(), F_OK) != 0);
    assert(ids() == "a,");
}

int main() {
    char tmpl[] = "/tmp/eventstoreXXXXXX";
    dir = mkdtemp(tmpl);
    testBasics();
    testLastFiredIsKept();
    testRemove();
    testCapacity();
    testPersistence();
    testStagedReplace();
    testDamagedFiles();
    std::string cleanup = "rm -rf " + dir;
    system(cleanup.c_str());
    printf("event store ok\n");
    return 0;
}
