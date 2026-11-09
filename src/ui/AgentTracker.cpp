// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/AgentTracker.h"

#include "attention.h"

namespace AgentTracker {
namespace {
AgentSessions::Table g_table;
}

auto table() -> AgentSessions::Table& { return g_table; }

void onHook(Attention::Agent agent, const Attention::HookEvent& event) {
    AgentSessions::apply(g_table, agent, event, AttentionQueue::now());
}

void expire() { AgentSessions::expire(g_table, AttentionQueue::now()); }

auto revision() -> uint32_t { return g_table.revision; }
}  // namespace AgentTracker
