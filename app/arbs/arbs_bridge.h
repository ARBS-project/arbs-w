// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W bridge between Moonlight (session / gamepad / keyboard / main) and ARBS Core (B2 D8,
// docs/analysis/B2_DESIGN.md section 4). Static facade; no Core, Win32 or SDL types in this header so that the
// Moonlight files only gain one include and a few calls. Every entry point is a no-op before initEarly().
//
// Threads (design 2-3):
//   main (Qt/SDL loop)      : initEarly, gamepad add/remove, input, hotkeys, minimise, onMainThreadStreamLive, onStreamEnd
//   async connection thread : onStreamLive
//   common-c receive thread : onPcm, consume* (never block: Core only takes short locks)
#pragma once

#include <QString>
#include <cstdint>

// SDL user event code posted by onStreamLive() for the main thread (session.cpp handles it).
#define ARBS_SDL_CODE_STREAM_LIVE 190

class StreamingPreferences;

class ArbsBridge {
public:
    // ---- main.cpp ----
    // Before the portable.dat check: if the exe folder has portable.dat and the working folder differs, switch the
    // working folder to the exe folder (prevents HKCU/%LOCALAPPDATA% writes when started from a shortcut).
    static void fixPortableWorkingDir();
    // First thing in main(), before any SDL call: SDL_HIDAPI_IGNORE_DEVICES += 0x37d7/0x2502 and
    // SDL_JOYSTICK_HIDAPI_FLYDIGI=0 as process environment variables and as OVERRIDE hints (design 4-4).
    static void applySdlHintsEarly();
    // After logging is set up (Path initialised): opens ARBS-<epoch>.log in the log folder, creates the engine.
    static void initEarly(const QString& logDir);
    static void shutdown(const char* reason);  // after app.exec(): stop sequence if needed, close IF2
    static void onCrash();                     // from the unhandled exception filter (try-locks only)

    // ---- session.cpp ----
    static void onStreamLive(const StreamingPreferences& prefs);  // async thread, right after LiStartConnection() == 0
    static void onMainThreadStreamLive(bool sendPendingArrival);  // ARBS_SDL_CODE_STREAM_LIVE on the main thread
    static void onStreamEnd(const char* reason);                  // main thread, before the input handler is deleted
    static void onWindowMinimized();
    static void onWindowRestored();
    // common-c callbacks. consume* return true when ARBS handled it (the caller must then skip the SDL path).
    static void onPcm(uint16_t controllerNumber, uint32_t sequence, const uint8_t* pcm, uint16_t frames);
    static bool consumeRumble(uint16_t controllerNumber, uint16_t lowFreq, uint16_t highFreq);
    static bool consumeRumbleTriggers(uint16_t controllerNumber, uint16_t left, uint16_t right);
    static bool consumeAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight,
                                        const uint8_t* left, const uint8_t* right);

    // ---- gamepad.cpp (main thread) ----
    // DEVICEADDED: identifies the APEX (design 4-3) and, if it is one, opens IF2 and attaches the engine.
    static bool onGamepadAdded(int playerIndex, int joystickId, uint16_t sdlVid, uint16_t sdlPid);
    static uint32_t apexCaps(uint32_t sdlCaps);
    static uint8_t apexType();
    // Arrival for the APEX goes through the stream-live gate (sent at most once per attach while live).
    static void sendApexArrival(int playerIndex, uint16_t activeGamepadMask, uint8_t type, uint32_t supportedButtons,
                                uint32_t capabilities);
    static void onGamepadRemoved(int joystickId);
    static bool isApexJoystick(int joystickId);
    static void onApexInput();
    static void onApexTriggerAxis(bool left, int sdlAxisValue);

    // ---- keyboard.cpp (Ctrl+Alt+Shift+H / J) ----
    static void toggleUserStop();
    static void reconnect();

    // ---- settings page ----
    static QString statusText();
    static QString logDir();
};
