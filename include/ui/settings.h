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
    // Weather (Open-Meteo, plain HTTP, no key). While it works the Home page replaces the plain clock.
    bool showWeather = true;
    float weatherLat = 51.5074f;       ///< default: London
    float weatherLon = -0.1278f;
    std::string weatherName = "London"; ///< label only (shown by the API and the web UI)
    bool useFahrenheit = false;

    // Night (see ui/night_core.h): when it starts, what the cube does, and how bright the face is
    std::string nightMode = "disabled";   ///< "disabled", "sunset", "schedule" or "manual"
    std::string nightStyle = "face";      ///< "face" (dim night clock, notifications still shown) or "off" (screen off)
    uint16_t nightStart = 1320;           ///< minutes after midnight (22:00): schedule start, and the fallback for sunset
    uint16_t nightEnd = 420;              ///< 07:00
    uint8_t nightBrightness = 10;         ///< backlight % of the night face
    bool nightManual = false;             ///< the state in "manual" mode (saved)

    bool showStatus = false;           ///< vendor status badges (not ported; kept for glimmer's screens)
    bool tlsVerify = true;             ///< validate server certificates (about 3 s per connection on this chip)

    // Runtime state, not persisted: filled from CredentialStore so the screens can tell what is configured
    bool claudeConfigured = false;
    bool codexConfigured = false;
    std::string codexModelLabel;       ///< status-bar label for the Codex screen, e.g. "GPT-5"
};
