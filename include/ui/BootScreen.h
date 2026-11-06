// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

/**
 * @brief The screens shown while the device starts and whenever there is no clock to show: connecting to Wi-Fi,
 *        online (network name and IP) and setup mode (the access point to join, with its password and IP).
 *
 * Each screen paints its static chrome once and only the chase dots animate afterwards (call tick()), so there is
 * no flicker. Styling follows the glimmer-derived theme and fonts.
 */
namespace BootScreen {

/** @brief "CONNECTING" with the network name and animated dots. Call repeatedly while waiting. */
void showConnecting(const char* ssid);

/**
 * @brief "ONLINE" with the network name, IP and signal strength, a one-line status note and animated dots.
 * @param note for example "starting up" or "waiting for time sync"
 */
void showOnline(const char* note);

/** @brief Setup mode: the access point to join (name and password) and the address to open. */
void showSetup(const char* apSsid, const char* apPassword, const char* staSsid);

/** @brief Repaint the setup screen from the values given to the last showSetup(). */
void redrawSetup();

/** @brief Animate the dots of the connecting / online screens. Cheap; call from the main loop. */
void tick(uint32_t nowMs);

/** @brief When the online screen first came up (millis()), 0 if it has not. */
auto onlineSinceMs() -> uint32_t;

}  // namespace BootScreen
