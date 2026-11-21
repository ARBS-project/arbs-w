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
#include <climits>
#include <cstdint>

// SDL user event code posted by onStreamLive() for the main thread (session.cpp handles it).
#define ARBS_SDL_CODE_STREAM_LIVE 190
// B2b 2-1: delayed APEX check (presumed match), posted by an SDL timer; data1 = binder generation.
#define ARBS_SDL_CODE_APEX_CHECK 191

// B2b: binding change that gamepad.cpp must report to the host (arrival / removal packets).
struct ArbsBind {
    static constexpr int kNone = INT_MIN;
    int boundJs = kNone;    // this joystick is now the APEX (PS type + PCM caps arrival needed)
    int unboundJs = kNone;  // this still-connected joystick is no longer the APEX (normal arrival needed)
};

class StreamingPreferences;

class ArbsBridge {
public:
    // ---- main.cpp ----
    // Before the portable.dat check: if the exe folder has portable.dat and the working folder differs, switch the
    // working folder to the exe folder (prevents HKCU/%LOCALAPPDATA% writes when started from a shortcut).
    static void fixPortableWorkingDir();
    // B4b F2 (D-B4b-1): hidden "--arbs-dump-prefs". Called right after the portable QSettings path is set, before the
    // ARBS log, the engine, SDL init and QGuiApplication. No portable.dat next to the exe -> returns 3 without reading
    // anything (no registry access). Otherwise reads StreamingPreferences::get() (never save()) and writes
    // <exe folder>\arbs-prefs-dump.txt (UTF-8, no BOM; arbsw::prefsDumpText) -> 0, write failure -> 4.
    static int dumpPrefs();
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
    // DEVICEADDED: identifies the APEX (B2b 2-1: exact VID/PID first, XInput fallback only after 1000 ms without an
    // exact pad, re-bind presumed -> exact). On a bind: IF2 opened, engine attached, trigger positions seeded.
    static ArbsBind onGamepadAdded(int playerIndex, int joystickId, uint16_t sdlVid, uint16_t sdlPid);
    // ARBS_SDL_CODE_APEX_CHECK (session.cpp -> SdlInputHandler): delayed presumed check.
    static ArbsBind onApexCheck(int generation);
    // Slot move (APEX first numbering): keeps the binder / engine controller number in step. B2c 2-3: when the bound
    // APEX number really changes, the arrival gate is re-opened so the arrival on the new number is sent.
    static void onGamepadIndexChanged(int joystickId, int playerIndex);
    static bool isExactPad(uint16_t sdlVid, uint16_t sdlPid);  // 37D7/2502
    static bool sdlListHasExact();                            // any 37D7/2502 device in the SDL joystick list
    static bool apexBound();
    // B2c 2-1: a 37D7/2502 game controller is in the SDL list and its DEVICEADDED has not been handled yet in this
    // session (it may still bind) -> APEX-only mode keeps the other pads hidden. A handled pad that did not bind is not
    // pending (analysis "exactBindFailed").
    static bool exactPending();
    static void log(const char* line);                        // one ARBS log line (UTF-8)
    static uint32_t apexCaps(uint32_t sdlCaps);
    static uint8_t apexType();
    // Arrival for the APEX goes through the stream-live gate (sent at most once per attach while live).
    static void sendApexArrival(int playerIndex, uint16_t activeGamepadMask, uint8_t type, uint32_t supportedButtons,
                                uint32_t capabilities);
    // DEVICEREMOVED: only the bound pad detaches the engine (2-4); then a remaining exact pad is bound at once.
    static ArbsBind onGamepadRemoved(int joystickId);
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
