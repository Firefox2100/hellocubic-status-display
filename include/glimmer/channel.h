// SPDX-License-Identifier: MIT
// Adapted from glimmer (https://github.com/Avinava/glimmer), Copyright (c) 2026 glimmer contributors.
// Used under the MIT license; see THIRD_PARTY.md.
#pragma once
#include <Arduino.h>

#include "ui/settings.h"
#include "usage_types.h"

// Context passed to every channel render.
struct ChannelCtx {
    const UiSettings* settings;
    const ClaudeData* claude;
    const CodexData*  codex;
    uint32_t now_ms;  // millis() at draw time
};

// A channel = name + enabled-predicate + full-draw + optional tick.
//
// PARTIAL REDRAW DISCIPLINE (the core architectural rule):
//
//   draw(ctx)  - Full repaint. Called once when the channel becomes active.
//                Calls Display::clear() and paints every pixel from scratch.
//
//   tick(ctx)  - Optional. Called at 5 Hz while the channel is active. MUST NOT
//                call Display::clear() or fillScreen. Only repaints the
//                specific pixel regions whose value changed since the last
//                tick/draw. This is how we avoid full-screen flashing on
//                time-varying content (the clock colon, countdown bars, ...).
struct Channel {
    const char* name;
    bool (*enabled)(const ChannelCtx&);
    void (*draw)(const ChannelCtx&);
    void (*tick)(const ChannelCtx&);  // may be nullptr
};
