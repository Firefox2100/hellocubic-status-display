// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

#include "agent_sessions.h"

/** @brief Device-side owner of the agent session table (logic in glimmer/agent_sessions.h). RAM only: a reboot forgets it. */
namespace AgentTracker {
auto table() -> AgentSessions::Table&;

/** @brief Record one hook event. */
void onHook(Attention::Agent agent, const Attention::HookEvent& event);

/** @brief Drop silent and long-finished sessions (call about once a second). */
void expire();

/** @brief Changes whenever a session changes state, appears or goes. */
auto revision() -> uint32_t;
}  // namespace AgentTracker
