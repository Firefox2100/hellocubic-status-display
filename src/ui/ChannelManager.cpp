// SPDX-License-Identifier: GPL-3.0-or-later
// Channel scheduling modelled on glimmer's main loop (https://github.com/Avinava/glimmer, MIT, see THIRD_PARTY.md).
#include "ui/ChannelManager.h"

#include <AnimatedGIF.h>
#include <LittleFS.h>
#include <Logger.h>
#include <strings.h>

#include <array>
#include <cstring>
#include <ctime>

#include "attention.h"
#include "net/UsageManager.h"
#include "ui/BootScreen.h"
#include "channel.h"
#include "display.h"
#include "display/DisplayManager.h"
#include "system/Diagnostics.h"
#include "ui/Presence.h"

// Channels implemented in src/glimmer/ch_*.cpp
extern auto chClockEnabled(const ChannelCtx& ctx) -> bool;
extern void chClockDraw(const ChannelCtx& ctx);
extern void chClockTick(const ChannelCtx& ctx);
extern auto chAttentionEnabled(const ChannelCtx& ctx) -> bool;
extern void chAttentionDraw(const ChannelCtx& ctx);
extern void chAttentionTick(const ChannelCtx& ctx);
extern auto chClaudeEnabled(const ChannelCtx& ctx) -> bool;
extern void chClaudeDraw(const ChannelCtx& ctx);
extern void chClaudeTick(const ChannelCtx& ctx);
extern auto chCodexEnabled(const ChannelCtx& ctx) -> bool;
extern void chCodexDraw(const ChannelCtx& ctx);
extern void chCodexTick(const ChannelCtx& ctx);
extern auto chAgentsEnabled(const ChannelCtx& ctx) -> bool;
extern void chAgentsDraw(const ChannelCtx& ctx);
extern void chAgentsTick(const ChannelCtx& ctx);

// Presence screens (src/ui/PresenceScreens.cpp)
extern void presenceBusyDraw(const ChannelCtx& ctx);
extern void presenceAwayDraw(const ChannelCtx& ctx);
extern auto presenceEnabledBusy(const ChannelCtx& ctx) -> bool;
extern auto presenceEnabledAway(const ChannelCtx& ctx) -> bool;

namespace {
constexpr const char* LOG_TAG = "ChannelManager";
constexpr const char* GIF_DIR = "/gif/";
constexpr uint32_t BOOT_ONLINE_MIN_MS = 6000;  // the online screen stays up at least this long
constexpr uint32_t BOOT_HOLD_FLOOR_MS = 1000;
constexpr uint32_t TICK_MS = 200;       // 5 Hz partial redraw
constexpr uint32_t INDICATOR_MS = 250;  // ~4 Hz
constexpr uint32_t EXPIRE_MS = 1000;
constexpr uint32_t GIF_RETRY_BLOCK_MS = 60000;
constexpr uint32_t MS_PER_SECOND = 1000;
constexpr uint32_t SECONDS_PER_MINUTE = 60;
constexpr long REASONABLE_EPOCH = 1000000000L;
// The GIF decoder is one ~24 KB allocation out of roughly 31 KB of heap. Starting it without leaving room for
// the web server and TCP buffers makes the device run out of memory and reset, so refuse instead.
constexpr uint32_t GIF_DECODER_BYTES = sizeof(AnimatedGIF);
constexpr uint32_t GIF_HEAP_HEADROOM = 4096;
constexpr int MAX_REDRAW_ATTEMPTS = 4;
constexpr size_t MAX_CHANNELS = 8;

UiSettings g_settings;
bool g_apMode = false;
bool g_started = false;
bool g_bootHold = false;
uint32_t g_bootHoldUntilMs = 0;

String g_manualGif;
bool g_gifAllowed = true;        // false after a crash reset, until the next clean boot
uint32_t g_gifBlockedUntil = 0;  // a GIF that failed is not retried for a while
bool g_drawFailed = false;       // set by a channel whose draw() could not show anything

std::array<int, MAX_CHANNELS> g_activeIdx{};
int g_activeCount = 0;
int g_activePtr = 0;
uint32_t g_lastSlide = 0;
bool g_wasAttention = false;
bool g_wasTimeSynced = false;

auto timeSynced() -> bool { return time(nullptr) > REASONABLE_EPOCH; }

auto sameName(const char* left, const char* right) -> bool { return strcmp(left, right) == 0; }

// ── Idle GIF channel ────────────────────────────────────────────────────────

auto gifPath() -> String {
    if (!g_manualGif.isEmpty()) {
        return g_manualGif;
    }
    if (Presence::mode() == Presence::Mode::Relaxing && !Presence::gif().isEmpty()) {
        return String(GIF_DIR) + Presence::gif();
    }
    if (!g_settings.idleGif.empty()) {
        return String(GIF_DIR) + g_settings.idleGif.c_str();
    }

    return {};
}

auto chGifEnabled(const ChannelCtx& ctx) -> bool {
    if (!g_manualGif.isEmpty()) {
        return true;
    }
    const bool relaxing = Presence::mode() == Presence::Mode::Relaxing;
    if (!relaxing && ctx.settings->idleMode != "gif" && ctx.settings->idleMode != "cycle") {
        return false;
    }
    if (!g_gifAllowed || (!relaxing && g_settings.idleGif.empty()) ||
        (relaxing && Presence::gif().isEmpty() && g_settings.idleGif.empty())) {
        return false;
    }

    return static_cast<int32_t>(millis() - g_gifBlockedUntil) >= 0;
}

void blockGif(const char* why) {
    Logger::warn(why, LOG_TAG);
    g_gifBlockedUntil = millis() + GIF_RETRY_BLOCK_MS;
    g_drawFailed = true;
    if (!g_manualGif.isEmpty()) {
        g_manualGif = "";  // a failed manual preview must not stick
    }
}

void chGifDraw([[maybe_unused]] const ChannelCtx& ctx) {
    const String path = gifPath();
    if (path.isEmpty() || !LittleFS.exists(path)) {
        blockGif("Idle GIF missing");

        return;
    }

    Display::releaseFont();  // frees a few KB of heap before the decoder allocates
    const uint32_t freeHeap = ESP.getFreeHeap();          // NOLINT(readability-static-accessed-through-instance)
    const uint32_t maxBlock = ESP.getMaxFreeBlockSize();  // NOLINT(readability-static-accessed-through-instance)
    if (maxBlock < GIF_DECODER_BYTES || freeHeap < GIF_DECODER_BYTES + GIF_HEAP_HEADROOM) {
        blockGif((String("GIF skipped, low heap: free=") + freeHeap + " block=" + maxBlock +
                  " need=" + (GIF_DECODER_BYTES + GIF_HEAP_HEADROOM))
                     .c_str());

        return;
    }

    if (!DisplayManager::playGifFullScreen(path)) {
        blockGif("GIF failed to start");
    }
}

void chGifTick([[maybe_unused]] const ChannelCtx& ctx) {
    // The GIF ended by itself (read error, file removed): do not leave a frozen frame.
    if (!DisplayManager::isGifPlaying()) {
        blockGif("Idle GIF stopped unexpectedly");
    }
}

// ── Channel table ───────────────────────────────────────────────────────────

const std::array<Channel, 8> kChannels = {{
    {"Busy", presenceEnabledBusy, presenceBusyDraw, nullptr},
    {"Away", presenceEnabledAway, presenceAwayDraw, nullptr},
    {"Attention", chAttentionEnabled, chAttentionDraw, chAttentionTick},
    {"Clock", chClockEnabled, chClockDraw, chClockTick},
    {"Claude", chClaudeEnabled, chClaudeDraw, chClaudeTick},
    {"Codex", chCodexEnabled, chCodexDraw, chCodexTick},
    {"Gif", chGifEnabled, chGifDraw, chGifTick},
    {"Agents", chAgentsEnabled, chAgentsDraw, chAgentsTick},
}};

auto makeCtx() -> ChannelCtx {
    return {&g_settings, &UsageManager::claude(), &UsageManager::codex(), millis()};
}

auto currentName() -> const char* {
    if (g_activeCount == 0) {
        return nullptr;
    }

    return kChannels.at(static_cast<size_t>(g_activeIdx.at(static_cast<size_t>(g_activePtr)))).name;
}

auto isGifActive() -> bool {
    const char* name = currentName();

    return name != nullptr && sameName(name, "Gif");
}

void recomputeActive() {
    // Keep showing the same channel across a recompute when it is still enabled.
    const char* cur = currentName();
    const ChannelCtx ctx = makeCtx();
    g_activeCount = 0;
    // Presence decides which family of channels may run; an attention card always can.
    const Presence::Mode presence = Presence::mode();
    const bool gifUsable = chGifEnabled(ctx);
    for (size_t i = 0; i < kChannels.size(); i++) {
        const char* chName = kChannels.at(i).name;
        bool allowed = true;
        if (!sameName(chName, "Attention")) {
            if (presence == Presence::Mode::Normal) {
                allowed = true;
            } else if (presence == Presence::Mode::Relaxing) {
                // the GIF, or the clock when the GIF cannot run (low heap, missing file)
                allowed = sameName(chName, "Gif") || (sameName(chName, "Clock") && !gifUsable);
            } else {
                allowed = sameName(chName, presence == Presence::Mode::Busy ? "Busy" : "Away");
            }
        }
        if (allowed && kChannels.at(i).enabled(ctx)) {
            g_activeIdx.at(static_cast<size_t>(g_activeCount)) = static_cast<int>(i);
            g_activeCount++;
        }
    }
    g_activePtr = 0;
    for (int i = 0; cur != nullptr && i < g_activeCount; i++) {
        const char* name = kChannels.at(static_cast<size_t>(g_activeIdx.at(static_cast<size_t>(i)))).name;
        if (sameName(name, cur)) {
            g_activePtr = i;
            break;
        }
    }
}

// Nothing to rotate (no time yet) or setup mode: show the connection details instead of a blank screen.
void drawInfo() {
    DisplayManager::stopGif(false);
    if (g_apMode) {
        BootScreen::redrawSetup();
    } else {
        BootScreen::showOnline(timeSynced() ? "" : "waiting for time sync");
    }
}

// Repaint the active channel. A channel that cannot show anything (GIF refused) is skipped.
void drawActive() {
    for (int attempt = 0; attempt < MAX_REDRAW_ATTEMPTS; attempt++) {
        if (g_activeCount == 0) {
            drawInfo();

            return;
        }
        g_drawFailed = false;
        // Switching away from a GIF releases the decoder (and its 24 KB) before fonts load.
        if (!isGifActive() && DisplayManager::isGifPlaying()) {
            DisplayManager::stopGif(false);
        }
        const auto channel = static_cast<size_t>(g_activeIdx.at(static_cast<size_t>(g_activePtr)));
        kChannels.at(channel).draw(makeCtx());
        g_lastSlide = millis();
        if (!g_drawFailed) {
            return;
        }
        recomputeActive();
    }
}

// Recompute, and repaint only when what should be on screen changed.
void refreshScreen(bool force) {
    const char* before = currentName();
    const int countBefore = g_activeCount;
    recomputeActive();
    const char* after = currentName();
    if (force || before != after || countBefore != g_activeCount) {
        drawActive();
    }
}

// 2-px strip at y=230. An agent waiting on the user (approval: amber, question: sky) turns it into a solid bar on
// EVERY screen, so "something is waiting" is visible even when it is not pinned. Otherwise it fills in the
// channel's colour as the rotation window elapses. Not drawn over a full-screen GIF.
void drawIndicator(uint32_t now) {
    using namespace Layout;
    if (g_apMode || g_activeCount == 0 || isGifActive()) {
        return;
    }
    const Attention::Queue& queue = AttentionQueue::get();
    if (Attention::countWaiting(queue, AttentionQueue::now()) > 0) {
        const bool approval = Attention::countKind(queue, Attention::K_APPROVAL) > 0;
        tft.fillRect(0, INDICATOR_Y, SCREEN_W, INDICATOR_H, approval ? Theme::AMBER : Theme::SKY);

        return;
    }
    const char* name = currentName();
    if (sameName(name, "Attention")) {
        tft.fillRect(0, INDICATOR_Y, SCREEN_W, INDICATOR_H, Theme::BG);

        return;
    }
    if (g_activeCount <= 1) {
        return;
    }

    int width = 0;
    if (g_settings.autoRotate) {
        const uint32_t slideMs = static_cast<uint32_t>(g_settings.channelSec) * MS_PER_SECOND;
        uint32_t elapsed = now - g_lastSlide;
        if (elapsed > slideMs) {
            elapsed = slideMs;
        }
        width = static_cast<int>(static_cast<uint64_t>(SCREEN_W) * elapsed / slideMs);
    }
    tft.fillRect(0, INDICATOR_Y, SCREEN_W, INDICATOR_H, Theme::PANEL);
    if (width > 0) {
        tft.fillRect(0, INDICATOR_Y, width, INDICATOR_H, Theme::channelColor(name));
    }
}

void attentionStep(uint32_t now) {
    static uint32_t lastExpire = 0;
    if (now - lastExpire >= EXPIRE_MS) {
        lastExpire = now;
        AttentionQueue::expire();
    }

    // A new interrupt card (or one that became more urgent) takes the screen right away; when nothing holds the
    // screen any more, the rotation resumes immediately.
    const ChannelCtx ctx = makeCtx();
    const bool attention = chAttentionEnabled(ctx);
    const bool interrupt = AttentionQueue::takeInterrupt();
    const char* curName = currentName() != nullptr ? currentName() : "";
    if (attention && (!g_wasAttention || (interrupt && !sameName(curName, "Attention")))) {
        ChannelManager::showChannel("Attention");
    } else if (!attention && g_wasAttention) {
        recomputeActive();
        drawActive();
    }
    g_wasAttention = attention;
}

void rotationStep(uint32_t now) {
    const char* name = currentName();
    const bool holding = g_wasAttention && name != nullptr && sameName(name, "Attention");
    const uint32_t slideMs = static_cast<uint32_t>(g_settings.channelSec) * MS_PER_SECOND;
    if (!g_settings.autoRotate || holding || g_activeCount <= 1 || !g_manualGif.isEmpty()) {
        return;
    }
    if (now - g_lastSlide < slideMs) {
        return;
    }

    g_lastSlide = now;
    recomputeActive();  // pick up settings toggles before deciding what is next
    if (g_activeCount > 0) {
        g_activePtr = (g_activePtr + 1) % g_activeCount;
    }
    drawActive();
}

void startChannels() {
    g_bootHold = false;
    refreshScreen(true);
}

void tickActive(uint32_t now) {
    static uint32_t lastTick = 0;
    if (g_activeCount == 0 || now - lastTick < TICK_MS) {
        return;
    }
    lastTick = now;
    const auto channel = static_cast<size_t>(g_activeIdx.at(static_cast<size_t>(g_activePtr)));
    auto tickFn = kChannels.at(channel).tick;
    if (tickFn != nullptr) {
        tickFn(makeCtx());
    }
    if (g_drawFailed) {  // a channel gave up (for example the GIF died): move on
        recomputeActive();
        drawActive();
    }
}
}  // namespace

// Accessors used by the attention channel (glimmer's main.cpp provides these there).
auto mainNightFace() -> bool { return false; }  // night face: not implemented yet
auto mainPinApprovals() -> bool { return g_settings.pinApprovals; }
auto mainAgentNightShow() -> bool { return g_settings.agentNightShow; }
auto mainApprovalTtlS() -> uint32_t { return static_cast<uint32_t>(g_settings.approvalTtlMin) * SECONDS_PER_MINUTE; }
auto mainAgentDoneCards() -> bool { return g_settings.agentDoneCards; }

void ChannelManager::begin(bool apMode) {
    g_apMode = apMode;
    g_started = true;
    g_gifAllowed = !Diagnostics::lastResetWasCrash();
    if (!g_gifAllowed) {
        Logger::warn("Previous reset was a crash: idle GIFs disabled until next clean boot", LOG_TAG);
    }
    g_activeCount = 0;
    g_bootHold = !apMode;
    const uint32_t now = millis();
    const uint32_t onlineUntil = BootScreen::onlineSinceMs() + BOOT_ONLINE_MIN_MS;
    g_bootHoldUntilMs = static_cast<int32_t>(onlineUntil - (now + BOOT_HOLD_FLOOR_MS)) > 0 ? onlineUntil
                                                                                           : now + BOOT_HOLD_FLOOR_MS;
}

void ChannelManager::setApMode(bool apMode) {
    if (apMode == g_apMode) {
        return;
    }
    g_apMode = apMode;
    if (g_started && !g_bootHold) {
        refreshScreen(true);  // leaving setup mode: back to the channels (or the "online" screen until time is set)
    }
}

void ChannelManager::applySettings(const UiSettings& settings) {
    g_settings = settings;
    g_manualGif = "";
    g_gifBlockedUntil = 0;
    if (g_started && !g_bootHold && !g_apMode) {
        refreshScreen(true);
    }
}

void ChannelManager::invalidate() {
    if (g_started && !g_bootHold) {
        drawActive();
    }
}

void ChannelManager::update() {
    // Advance the GIF decoder (non-blocking, one frame at most per call)
    DisplayManager::update();
    if (!g_started) {
        return;
    }

    const uint32_t now = millis();
    if (g_bootHold) {
        if (static_cast<int32_t>(now - g_bootHoldUntilMs) < 0) {
            BootScreen::tick(now);  // keep the dots moving while the boot screen is up

            return;
        }
        startChannels();
    }
    if (g_apMode && g_manualGif.isEmpty()) {
        return;
    }

    // The clock channel only exists once time is synced, and the usage channels come and go with their credentials
    // and data: both change on their own, so watch for them.
    static uint32_t lastSyncCheck = 0;
    static uint32_t seenUsageRevision = 0;
    static uint32_t seenPresenceRevision = 0;
    if (Presence::revision() != seenPresenceRevision) {
        seenPresenceRevision = Presence::revision();
        g_manualGif = "";
        g_gifBlockedUntil = 0;
        refreshScreen(true);
    }
    if (now - lastSyncCheck >= EXPIRE_MS) {
        lastSyncCheck = now;
        const bool synced = timeSynced();
        const uint32_t usageRevision = UsageManager::revision();
        if (synced != g_wasTimeSynced || usageRevision != seenUsageRevision) {
            g_wasTimeSynced = synced;
            seenUsageRevision = usageRevision;
            refreshScreen(false);
        }
    }

    if (!g_apMode) {
        attentionStep(now);
    }
    rotationStep(now);
    tickActive(now);

    static uint32_t lastIndicator = 0;
    if (now - lastIndicator >= INDICATOR_MS) {
        lastIndicator = now;
        drawIndicator(now);
    }
}

auto ChannelManager::playIdleGif(const String& path) -> bool {
    g_manualGif = path;
    g_bootHold = false;
    g_gifBlockedUntil = 0;
    recomputeActive();

    return showChannel("Gif") && g_manualGif.length() > 0;
}

auto ChannelManager::stopIdleGif() -> bool {
    g_manualGif = "";
    g_bootHold = false;
    if (g_started) {
        refreshScreen(true);
    }

    return true;
}

void ChannelManager::releaseMemory() {
    if (!DisplayManager::isGifPlaying()) {
        return;
    }
    DisplayManager::stopGif(false);
    g_gifBlockedUntil = millis() + GIF_RETRY_BLOCK_MS;  // chGifTick notices and moves on to another channel
}

auto ChannelManager::activeName() -> const char* {
    const char* name = currentName();

    return name != nullptr ? name : "Info";
}

auto ChannelManager::showChannel(const char* name) -> bool {
    recomputeActive();
    for (int i = 0; i < g_activeCount; i++) {
        const char* candidate = kChannels.at(static_cast<size_t>(g_activeIdx.at(static_cast<size_t>(i)))).name;
        if (strcasecmp(candidate, name) == 0) {
            g_activePtr = i;
            drawActive();

            return true;
        }
    }

    return false;
}

void ChannelManager::nextChannel() {
    recomputeActive();
    if (g_activeCount == 0) {
        return;
    }
    g_activePtr = (g_activePtr + 1) % g_activeCount;
    drawActive();
}
