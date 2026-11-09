// SPDX-License-Identifier: GPL-3.0-or-later
// Which coding-agent sessions are running, waiting for you, or finished. Pure logic (no Arduino), host-tested.
//
// A session is identified by the agent plus a hash of its FULL session id (not a prefix: Codex ids are time-ordered, so
// the first characters are shared by sessions started close together). Every hook event carries the session id, so a
// "Stop" from one session can only ever mark that session finished.
#pragma once
#include <stdint.h>
#include <string.h>

#include "attention_core.h"

namespace AgentSessions {

constexpr int kMax = 6;                        // sessions tracked at once
constexpr uint32_t kStaleS = 30 * 60;          // working / waiting with no event for this long is dropped (crashed session)
constexpr uint32_t kDoneKeepS = 15 * 60;       // a finished session stays listed this long, so you can go back to it
constexpr size_t kIdLen = 9;                   // 8 hex digits + NUL

enum State : uint8_t { S_FREE = 0, S_WORKING, S_WAITING, S_DONE };

struct Session {
    State state = S_FREE;
    Attention::Agent agent = Attention::AGENT_OTHER;
    char id[kIdLen] = "";
    char project[16] = "";
    uint32_t startedS = 0;   // when the current run began (last prompt)
    uint32_t changedS = 0;   // last state change
    uint32_t lastS = 0;      // last event of any kind
};

struct Table {
    Session s[kMax];
    uint32_t revision = 1;
};

/** @brief FNV-1a of the whole session id as 8 hex digits ("default" for an empty id). */
inline void idOf(const char* session, char* out) {
    const char* text = (session != nullptr && *session != '\0') ? session : "default";
    uint32_t hash = 2166136261U;
    for (const char* p = text; *p != '\0'; p++) {
        hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619U;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out[i] = hex[hash & 0xFU];
        hash >>= 4;
    }
    out[8] = '\0';
}

inline Session* find(Table& t, Attention::Agent agent, const char* id) {
    for (Session& s : t.s) {
        if (s.state != S_FREE && s.agent == agent && strcmp(s.id, id) == 0) return &s;
    }
    return nullptr;
}

// Make room: a free slot, else the oldest finished session, else the least recently heard from.
inline Session* slotFor(Table& t) {
    Session* pick = nullptr;
    for (Session& s : t.s) {
        if (s.state == S_FREE) return &s;
        const bool better = pick == nullptr || (s.state == S_DONE && pick->state != S_DONE) ||
                            ((s.state == S_DONE) == (pick->state == S_DONE) && s.lastS < pick->lastS);
        if (better) pick = &s;
    }
    return pick;
}

/**
 * @brief Apply one hook event. Returns true when anything visible changed.
 *
 * - UserPromptSubmit / PostToolUse(Failure): working (a PostToolUse also means a pending approval was answered)
 * - permission prompt, question, Codex PermissionRequest: waiting for you
 * - Stop / agent_completed: done
 * - SessionEnd: the session is forgotten
 */
inline bool apply(Table& t, Attention::Agent agent, const Attention::HookEvent& e, uint32_t now) {
    const char* ev = e.event != nullptr ? e.event : "";
    const char* ty = e.type != nullptr ? e.type : "";
    State next = S_FREE;
    bool remove = false;
    if (!strcmp(ev, "UserPromptSubmit") || !strcmp(ev, "PostToolUse") || !strcmp(ev, "PostToolUseFailure")) {
        next = S_WORKING;
    } else if (!strcmp(ev, "PermissionRequest") ||
               (!strcmp(ev, "Notification") && (!strcmp(ty, "permission_prompt") || !strcmp(ty, "idle_prompt") ||
                                                !strcmp(ty, "agent_needs_input") || !strcmp(ty, "elicitation_dialog")))) {
        next = S_WAITING;
    } else if (!strcmp(ev, "Stop") || (!strcmp(ev, "Notification") && !strcmp(ty, "agent_completed"))) {
        next = S_DONE;
    } else if (!strcmp(ev, "SessionEnd")) {
        remove = true;
    } else {
        return false;
    }

    char id[kIdLen];
    idOf(e.session, id);
    Session* s = find(t, agent, id);
    if (remove) {
        if (s == nullptr) return false;
        *s = Session();
        t.revision++;

        return true;
    }
    if (s == nullptr) {
        s = slotFor(t);
        if (s == nullptr) return false;
        *s = Session();
        s->agent = agent;
        strncpy(s->id, id, kIdLen - 1);
        s->startedS = now;
        s->changedS = now;
    }
    Attention::basename(e.cwd, s->project, sizeof(s->project));
    s->lastS = now;
    bool changed = false;
    if (s->state != next) {
        if (next == S_WORKING && s->state == S_DONE) s->startedS = now;  // a new run in the same session
        s->state = next;
        s->changedS = now;
        changed = true;
    } else if (next == S_WORKING && ev[0] == 'U') {
        s->startedS = now;  // a fresh prompt while already working restarts the run clock
    }
    if (changed) t.revision++;

    return changed;
}

/** @brief Drop sessions nobody has heard from (a crashed agent) and finished ones past their keep time. */
inline int expire(Table& t, uint32_t now) {
    int dropped = 0;
    for (Session& s : t.s) {
        if (s.state == S_FREE) continue;
        const uint32_t idle = now - s.lastS;
        if ((s.state == S_DONE && idle > kDoneKeepS) || (s.state != S_DONE && idle > kStaleS)) {
            s = Session();
            dropped++;
        }
    }
    if (dropped > 0) t.revision++;

    return dropped;
}

inline void counts(const Table& t, int& working, int& waiting, int& done) {
    working = waiting = done = 0;
    for (const Session& s : t.s) {
        if (s.state == S_WORKING) working++;
        else if (s.state == S_WAITING) waiting++;
        else if (s.state == S_DONE) done++;
    }
}

inline const char* stateName(State s) {
    return s == S_WORKING ? "working" : s == S_WAITING ? "waiting" : s == S_DONE ? "done" : "free";
}

}  // namespace AgentSessions
