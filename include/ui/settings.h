// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

#include <string>

/**
 * @brief User-facing display settings (persisted in config.json under "ui", edited through /api/v1/idle and
 *        /api/v1/ui).
 *
 * Field names follow glimmer's Settings where the meaning is the same.
 */
struct UiSettings {
    std::string userName;              ///< shown in the clock greeting ("good morning, <name>")
    bool clock24h = true;
    std::string idleMode = "clock";    ///< "clock", "gif" or "cycle" (clock and GIF alternate)
    std::string idleGif;               ///< file name inside /gif used by the Gif channel
    uint16_t channelSec = 15;          ///< seconds each channel stays up while rotating
    bool autoRotate = true;
    bool pinApprovals = true;          ///< approvals/questions hold the screen until answered
    uint16_t approvalTtlMin = 30;      ///< safety expiry when nothing clears an approval
    bool agentNightShow = false;       ///< reserved for the night face (not implemented yet)
    bool agentDoneCards = false;       ///< short "DONE" card when an agent finishes (hook events)

    // Claude / Codex usage screens (credentials are in CredentialStore, never here)
    bool showClaude = true;
    bool showCodex = true;
    bool showTrend = true;             ///< 7-day burn per provider (needs history)
    bool showAiDash = true;            ///< both allowances side by side, with advice (needs both configured)
    bool showInfo = false;             ///< diagnostics page: signal, memory, uptime
    uint16_t refreshMin = 5;           ///< minutes between usage fetches
    bool claudeWeeklyHero = false;     ///< weekly window as the big number instead of the 5-hour one
    bool codexWeeklyHero = false;
    bool showStatus = false;           ///< vendor status badges (not ported; kept for glimmer's screens)
    bool tlsVerify = true;             ///< validate server certificates (about 3 s per connection on this chip)

    // Runtime state, not persisted: filled from CredentialStore so the screens can tell what is configured
    bool claudeConfigured = false;
    bool codexConfigured = false;
    std::string codexModelLabel;       ///< status-bar label for the Codex screen, e.g. "GPT-5"
};
