// Host test for the agent session table (include/glimmer/agent_sessions.h).
#include <assert.h>
#include <stdio.h>

#include <string>

#include "agent_sessions.h"

using namespace AgentSessions;
using Attention::AGENT_CLAUDE;
using Attention::AGENT_CODEX;
using Attention::HookEvent;

static HookEvent ev(const char* event, const char* session, const char* type = "", const char* cwd = "/home/me/proj") {
    HookEvent e;
    e.event = event;
    e.session = session;
    e.type = type;
    e.cwd = cwd;
    return e;
}

static const Session* get(Table& t, Attention::Agent a, const char* session) {
    char id[kIdLen];
    idOf(session, id);
    return find(t, a, id);
}

int main() {
    // ids: whole-string hash, so time-ordered ids with a shared prefix still differ
    char a[kIdLen], b[kIdLen], c[kIdLen];
    idOf("0190f3a2-1111-7000-8000-aaaaaaaaaaaa", a);
    idOf("0190f3a2-1111-7000-8000-bbbbbbbbbbbb", b);
    idOf("0190f3a2-1111-7000-8000-aaaaaaaaaaaa", c);
    assert(strcmp(a, b) != 0 && strcmp(a, c) == 0 && strlen(a) == 8);
    char cardId[24];
    Attention::sessionId(AGENT_CODEX, "0190f3a2-1111-7000-8000-aaaaaaaaaaaa", cardId, sizeof(cardId));
    assert(strcmp(cardId, (std::string("codex:") + a).c_str()) == 0);  // card ids and session ids agree

    Table t;
    // two sessions of the same agent start working
    assert(apply(t, AGENT_CLAUDE, ev("UserPromptSubmit", "s1", "", "/work/alpha"), 100));
    assert(apply(t, AGENT_CLAUDE, ev("UserPromptSubmit", "s2", "", "/work/beta"), 105));
    assert(get(t, AGENT_CLAUDE, "s1")->state == S_WORKING && !strcmp(get(t, AGENT_CLAUDE, "s1")->project, "alpha"));
    // the same id under another agent is a different session
    assert(apply(t, AGENT_CODEX, ev("UserPromptSubmit", "s1"), 110));
    int w, q, d;
    counts(t, w, q, d);
    assert(w == 3 && q == 0 && d == 0);

    // s1 finishes: only s1 is done, s2 and the codex s1 keep working
    assert(apply(t, AGENT_CLAUDE, ev("Stop", "s1"), 200));
    assert(get(t, AGENT_CLAUDE, "s1")->state == S_DONE);
    assert(get(t, AGENT_CLAUDE, "s2")->state == S_WORKING);
    assert(get(t, AGENT_CODEX, "s1")->state == S_WORKING);

    // s2 asks for approval, then the approved tool finishes -> working again
    assert(apply(t, AGENT_CLAUDE, ev("Notification", "s2", "permission_prompt"), 210));
    assert(get(t, AGENT_CLAUDE, "s2")->state == S_WAITING);
    assert(apply(t, AGENT_CLAUDE, ev("PostToolUse", "s2"), 220));
    assert(get(t, AGENT_CLAUDE, "s2")->state == S_WORKING);
    // a repeated event changes nothing visible
    assert(!apply(t, AGENT_CLAUDE, ev("PostToolUse", "s2"), 221));

    // a new prompt in a finished session starts a new run
    assert(apply(t, AGENT_CLAUDE, ev("UserPromptSubmit", "s1"), 300));
    assert(get(t, AGENT_CLAUDE, "s1")->state == S_WORKING && get(t, AGENT_CLAUDE, "s1")->startedS == 300);

    // unknown events are ignored; SessionEnd forgets only that session
    assert(!apply(t, AGENT_CLAUDE, ev("SessionStart", "s9"), 301));
    assert(get(t, AGENT_CLAUDE, "s9") == nullptr);
    assert(apply(t, AGENT_CLAUDE, ev("SessionEnd", "s2"), 302));
    assert(get(t, AGENT_CLAUDE, "s2") == nullptr && get(t, AGENT_CLAUDE, "s1") != nullptr);
    assert(!apply(t, AGENT_CLAUDE, ev("SessionEnd", "s2"), 303));

    // expiry: a silent working session is dropped after kStaleS, a done one after kDoneKeepS
    Table e;
    apply(e, AGENT_CLAUDE, ev("UserPromptSubmit", "x"), 1000);
    apply(e, AGENT_CLAUDE, ev("Stop", "y"), 1000);
    assert(expire(e, 1000 + kDoneKeepS) == 0);
    assert(expire(e, 1000 + kDoneKeepS + 1) == 1 && get(e, AGENT_CLAUDE, "y") == nullptr && get(e, AGENT_CLAUDE, "x") != nullptr);
    assert(expire(e, 1000 + kStaleS + 1) == 1 && get(e, AGENT_CLAUDE, "x") == nullptr);

    // full table: a new session evicts a finished one first, never a live one
    Table f;
    for (int i = 0; i < kMax; i++) {
        char sid[8];
        snprintf(sid, sizeof(sid), "w%d", i);
        apply(f, AGENT_CLAUDE, ev("UserPromptSubmit", sid), 10 + i);
    }
    apply(f, AGENT_CLAUDE, ev("Stop", "w3"), 50);
    assert(apply(f, AGENT_CLAUDE, ev("UserPromptSubmit", "new"), 60));
    assert(get(f, AGENT_CLAUDE, "w3") == nullptr && get(f, AGENT_CLAUDE, "new") != nullptr);
    assert(get(f, AGENT_CLAUDE, "w0") != nullptr);

    // done cards: queued (not interrupting), named after the project, same id as the session's card
    Attention::Item card, extra;
    HookEvent stop = ev("Stop", "s1", "", "/work/alpha");
    assert(Attention::fromHook(AGENT_CLAUDE, stop, true, 1800, 5000, card, &extra) == Attention::HOOK_UPSERT);
    assert(card.kind == Attention::K_SUCCESS && card.display == Attention::SHOW_QUEUE && !strcmp(card.body, "alpha finished"));
    assert(card.expires == 5000 + Attention::kDoneCardS);

    printf("agent_sessions_test: ok\n");
    return 0;
}
