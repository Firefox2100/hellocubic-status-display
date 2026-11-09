// SPDX-License-Identifier: GPL-3.0-or-later
#include "display/CgfPlayer.h"

#include <LittleFS.h>
#include <Logger.h>

#include "display/DisplayManager.h"
#include "display/cgf.h"

namespace {
constexpr const char* LOG_TAG = "CgfPlayer";
constexpr int SCREEN_SIZE = 240;
constexpr uint32_t MAX_FRAME_LAG_MS = 200;  // after a stall, do not fast-forward through a backlog of frames

class FileSource : public Cgf::Source {
   public:
    explicit FileSource(File file) : m_file(std::move(file)) {}
    auto read(uint8_t* dst, size_t len) -> size_t override { return m_file.read(dst, len); }
    auto seek(uint32_t pos) -> bool override { return m_file.seek(pos, SeekSet); }

   private:
    File m_file;
};

class PanelSink : public Cgf::Sink {
   public:
    void setOffset(int offX, int offY) {
        m_offX = offX;
        m_offY = offY;
    }
    void span(int xPos, int yPos, int len, const uint16_t* pixels) override {
        tft.setAddrWindow(m_offX + xPos, m_offY + yPos, len, 1);
        tft.pushColors(const_cast<uint16_t*>(pixels), static_cast<uint32_t>(len), true);
    }

   private:
    int m_offX = 0;
    int m_offY = 0;
};

FileSource* g_source = nullptr;
Cgf::Player* g_player = nullptr;
PanelSink g_sink;
uint32_t g_nextAt = 0;
}  // namespace

auto CgfPlayer::isCgf(const String& path) -> bool { return path.endsWith(".cgf") || path.endsWith(".CGF"); }

auto CgfPlayer::start(const String& path) -> bool {
    stop();
    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }
    g_source = new FileSource(std::move(file));
    g_player = new Cgf::Player();
    if (!g_player->open(*g_source)) {
        Logger::warn(F("Not a usable CGF1 file"), LOG_TAG);
        stop();

        return false;
    }
    g_sink.setOffset((SCREEN_SIZE - g_player->width()) / 2, (SCREEN_SIZE - g_player->height()) / 2);
    DisplayManager::clearScreen();
    g_nextAt = millis();

    return true;
}

void CgfPlayer::stop() {
    delete g_player;
    g_player = nullptr;
    delete g_source;
    g_source = nullptr;
}

auto CgfPlayer::playing() -> bool { return g_player != nullptr; }

void CgfPlayer::update() {
    if (g_player == nullptr || static_cast<int32_t>(millis() - g_nextAt) < 0) {
        return;
    }
    tft.startWrite();
    const uint16_t duration = g_player->nextFrame(g_sink);
    tft.endWrite();
    if (duration == 0) {
        Logger::warn(F("CGF1 file damaged, stopping"), LOG_TAG);
        stop();

        return;
    }
    const uint32_t now = millis();
    g_nextAt += duration;
    if (static_cast<int32_t>(now - g_nextAt) > static_cast<int32_t>(MAX_FRAME_LAG_MS)) {
        g_nextAt = now;
    }
}
