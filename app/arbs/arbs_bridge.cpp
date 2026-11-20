// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W bridge implementation. See arbs_bridge.h and docs/analysis/B2_DESIGN.md section 4.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "arbs_bridge.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <Limelight.h>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "SDL_compat.h"
#include "arbs/engine.h"
#include "arbs/java_compat.h"
#include "arbs_log.h"
#include "arbs_logic.h"
#include "arbs_policy.h"
#include "arbs_settings.h"
#include "settings/streamingpreferences.h"
#include "win_clock.h"
#include "win_hid_port.h"

// The arrival constants must match moonlight-common-c (Limelight.h / ControllerHaptics.h).
static_assert(arbsw::kCapAnalogTriggers == LI_CCAP_ANALOG_TRIGGERS, "caps");
static_assert(arbsw::kCapRumble == LI_CCAP_RUMBLE, "caps");
static_assert(arbsw::kCapTriggerRumble == LI_CCAP_TRIGGER_RUMBLE, "caps");
static_assert(arbsw::kTypePs == LI_CTYPE_PS, "type");

namespace {

constexpr int kNoJoystick = INT_MIN;

struct PendingArrival {
    bool valid = false;
    int index = 0;
    uint16_t mask = 0;
    uint8_t type = 0;
    uint32_t buttons = 0;
    uint32_t caps = 0;
};

struct Impl {
    arbsw::WinClock clock;
    std::unique_ptr<arbsw::ArbsLogFile> log;
    std::unique_ptr<arbs::Engine> engine;
    arbsw::ArrivalGate gate;
    std::mutex pendingMu;
    PendingArrival pending;
    std::atomic<int> apexIndex{-1};
    std::atomic<int> apexJs{kNoJoystick};
    std::atomic<bool> minimizedPause{false};
    std::atomic<int64_t> triggerRumbleIgnored{0};
    int leftTrigger = 0, rightTrigger = 0;  // main thread only
    QString logDir;
};

Impl* g_impl = nullptr;
std::string g_earlyNotes;  // lines produced before the log file exists (portable fix, hints)

void logLine(const std::string& s) {
    if (g_impl != nullptr && g_impl->log) g_impl->log->info(s);
}

std::string q2s(const QString& s) { return s.toUtf8().toStdString(); }

arbsw::ArbsSettings settingsFrom(const StreamingPreferences& p) {
    arbsw::ArbsSettings s;
    s.amp = p.arbsAmp;
    s.gain = p.arbsGain;
    s.gatePct = p.arbsGate;
    s.silence = p.arbsSilence;
    s.curve = p.arbsCurve;
    s.taps = p.arbsTaps;
    s.jitterFixedMs = p.arbsJitterFixed;
    s.trigger = p.arbsTrigger;
    s.triggerStrength = p.arbsTriggerStrength;
    s.verbose = p.arbsVerboseLog;
    return arbsw::settings::normalize(s);
}

int countFallbackPads() {
    int n = 0;
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        if (SDL_JoystickGetDeviceVendor(i) == arbsw::kXInputFallbackVid && SDL_JoystickGetDeviceProduct(i) == arbsw::kXInputFallbackPid) n++;
    }
    return n;
}

void logJoystickList() {
    const int count = SDL_NumJoysticks();
    logLine("ARBS-W: SDL 조이스틱 " + std::to_string(count) + "개 (IF2 가 SDL HIDAPI 에 잡히지 않았는지 확인)");
    for (int i = 0; i < count; i++) {
        const char* name = SDL_JoystickNameForIndex(i);
        const char* path = SDL_JoystickPathForIndex(i);
        char vp[32];
        std::snprintf(vp, sizeof(vp), "%04x/%04x", SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i));
        logLine("ARBS-W: SDL 조이스틱[" + std::to_string(i) + "] " + (name ? name : "<null>") + " VID/PID " + vp + " 경로 " +
                (path ? path : "<null>"));
        if (path != nullptr && arbsw::pathLooksLikeIf2(path)) {
            logLine("ARBS-W 경고: SDL 이 동글 IF2(MI_02) 를 조이스틱으로 열었음 - SDL_HIDAPI_IGNORE_DEVICES 가 적용되지 않았을 수 있음");
        }
    }
    const char* ign = SDL_GetHint(SDL_HINT_HIDAPI_IGNORE_DEVICES);
    const char* fly = SDL_GetHint("SDL_JOYSTICK_HIDAPI_FLYDIGI");
    logLine(std::string("ARBS-W: SDL 힌트 확인 SDL_HIDAPI_IGNORE_DEVICES=") + (ign ? ign : "<null>") +
            " SDL_JOYSTICK_HIDAPI_FLYDIGI=" + (fly ? fly : "<null>"));
}

void sendPending() {
    PendingArrival a;
    {
        std::lock_guard<std::mutex> g(g_impl->pendingMu);
        a = g_impl->pending;
        g_impl->pending.valid = false;
    }
    if (!a.valid) return;
    const int rc = LiSendControllerArrivalEvent(static_cast<uint8_t>(a.index), a.mask, a.type, a.buttons, static_cast<uint16_t>(a.caps));
    logLine("ARBS-W: APEX 도착 보고(보류분, 스트림 연결 후) 번호 " + std::to_string(a.index) + " 타입 PS caps 0x" +
            arbs::jc::fmt("%x", a.caps) + " rc=" + std::to_string(rc));
}

}  // namespace

// ------------------------------------------------------------------------------------------------ main.cpp
void ArbsBridge::fixPortableWorkingDir() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    if (n == 0 || n >= sizeof(exe) / sizeof(exe[0])) return;
    std::wstring dir(exe, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.resize(slash);
    const std::wstring marker = dir + L"\\portable.dat";
    if (GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t cwd[MAX_PATH * 4];
    const DWORD c = GetCurrentDirectoryW(static_cast<DWORD>(sizeof(cwd) / sizeof(cwd[0])), cwd);
    if (c > 0 && _wcsicmp(cwd, dir.c_str()) == 0) return;
    if (SetCurrentDirectoryW(dir.c_str())) {
        g_earlyNotes += "ARBS-W: 포터블(portable.dat) 실행 - 작업 폴더를 exe 폴더로 맞춤(바로 가기 실행 대비)\n";
    } else {
        g_earlyNotes += "ARBS-W 경고: 작업 폴더를 exe 폴더로 바꾸지 못함(오류 " + std::to_string(GetLastError()) + ")\n";
    }
}

void ArbsBridge::applySdlHintsEarly() {
    // Process environment first (SDL reads hints from the environment too), then OVERRIDE hints, which win over any
    // environment value SDL may have captured earlier. Nothing outside this process is changed.
    const QByteArray oldIgnore = qgetenv("SDL_HIDAPI_IGNORE_DEVICES");
    const std::string merged = arbsw::appendIgnoreDevice(oldIgnore.toStdString(), arbsw::kIgnoreEntry);
    qputenv("SDL_HIDAPI_IGNORE_DEVICES", QByteArray::fromStdString(merged));
    qputenv("SDL_JOYSTICK_HIDAPI_FLYDIGI", "0");
    const bool h1 = SDL_SetHintWithPriority(SDL_HINT_HIDAPI_IGNORE_DEVICES, merged.c_str(), SDL_HINT_OVERRIDE) == SDL_TRUE;
    const bool h2 = SDL_SetHintWithPriority("SDL_JOYSTICK_HIDAPI_FLYDIGI", "0", SDL_HINT_OVERRIDE) == SDL_TRUE;
    g_earlyNotes += "ARBS-W: SDL 첫 사용 전 힌트 적용 SDL_HIDAPI_IGNORE_DEVICES=" + merged + "(기존 " +
                    (oldIgnore.isEmpty() ? std::string("없음") : oldIgnore.toStdString()) + ") 힌트 " + (h1 ? "OK" : "실패") +
                    ", SDL_JOYSTICK_HIDAPI_FLYDIGI=0 힌트 " + (h2 ? "OK" : "실패") + " (+ 프로세스 환경 변수)\n";
}

void ArbsBridge::initEarly(const QString& logDir) {
    if (g_impl != nullptr) return;
    g_impl = new Impl();
    g_impl->logDir = logDir;
    QDir dir(logDir);
    const QString name = QString("ARBS-%1.log").arg(QDateTime::currentSecsSinceEpoch());
    g_impl->log.reset(new arbsw::ArbsLogFile(QDir::toNativeSeparators(dir.filePath(name)).toStdWString(),
                                              [] { return g_impl->clock.nowNs(); }));
    g_impl->log->setMirror(arbsw::isStateLine, [](const std::string& s) { SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "ARBS: %s", s.c_str()); });
    if (!g_impl->log->ok()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "ARBS: log file could not be created in the log folder");
    }
    // Keep the 10 newest ARBS logs (same rule as the Moonlight logs).
    const QStringList old = dir.entryList(QStringList("ARBS-*.log"), QDir::Files, QDir::Time);
    for (int i = 10; i < old.size(); i++) QFile(dir.filePath(old.at(i))).remove();

    logLine(std::string("ARBS-W B2 로그 - Moonlight ") + VERSION_STR + " + ARBS Core (APEX 햅틱). IP·토큰·PIN 은 기록하지 않음");
    logLine(std::string("ARBS-W: 로그 폴더 = ") + (QFileInfo::exists(dir.filePath("portable.dat")) ? "포터블 폴더(exe 폴더)" : "임시 폴더"));
    size_t start = 0;
    while (start < g_earlyNotes.size()) {
        const size_t nl = g_earlyNotes.find('\n', start);
        logLine(g_earlyNotes.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    logLine(std::string("ARBS-W: 잠정 설정 - rumble ") + (arbsw::policy::kRumbleCoreOnly ? "Core 전용" : "Core(READY 아닐 때 XInput)") +
            ", 창 최소화 = " + (arbsw::policy::kPauseOnMinimize ? "APEX 정지(복원 시 재개)" : "영향 없음"));
    g_impl->engine.reset(new arbs::Engine(g_impl->clock, *g_impl->log));
}

void ArbsBridge::shutdown(const char* reason) {
    if (g_impl == nullptr || !g_impl->engine) return;
    try {
        g_impl->engine->onSessionEnd(reason);
        g_impl->engine->detach(reason);
        logLine(std::string("ARBS-W: 종료(") + reason + ") " + g_impl->engine->statusLine());
    } catch (...) {
    }
}

void ArbsBridge::onCrash() {
    if (g_impl == nullptr) return;
    try {
        if (g_impl->log) g_impl->log->setCrashMode();
        if (g_impl->engine) g_impl->engine->onCrash();
    } catch (...) {
    }
}

// ------------------------------------------------------------------------------------------------ session.cpp
void ArbsBridge::onStreamLive(const StreamingPreferences& prefs) {
    if (g_impl == nullptr) return;
    try {
        const arbsw::ArbsSettings s = settingsFrom(prefs);
        logLine("ARBS-W: 스트림 연결됨(LiStartConnection 성공) - streamLive");
        arbs::Engine& e = *g_impl->engine;
        // onSessionStart touches engine-thread state (jitter, counters): only call it while the engine is inactive
        // (B2_CORE_MAP.md section 3 #14).
        if (e.isActive()) {
            logLine("ARBS-W: 엔진이 아직 활성 - 이전 세션 정리 후 시작");
            e.onSessionEnd("이전 세션 정리");
        }
        if (!e.isActive()) {
            e.onSessionStart(arbsw::toEngineConfig(s));
        } else {
            logLine("ARBS-W 경고: 엔진이 비활성이 되지 않아 이번 세션 설정 적용을 건너뜀");
        }
        const bool flush = g_impl->gate.setLive();
        SDL_Event ev = {};
        ev.type = SDL_USEREVENT;
        ev.user.code = ARBS_SDL_CODE_STREAM_LIVE;
        ev.user.data1 = reinterpret_cast<void*>(static_cast<uintptr_t>(flush ? 1 : 0));
        SDL_PushEvent(&ev);
    } catch (...) {
    }
}

void ArbsBridge::onMainThreadStreamLive(bool sendPendingArrival) {
    if (g_impl == nullptr) return;
    logJoystickList();
    if (sendPendingArrival) sendPending();
}

void ArbsBridge::onStreamEnd(const char* reason) {
    if (g_impl == nullptr) return;
    try {
        g_impl->gate.reset();
        {
            std::lock_guard<std::mutex> g(g_impl->pendingMu);
            g_impl->pending.valid = false;
        }
        arbs::Engine& e = *g_impl->engine;
        e.onSessionEnd(reason);  // stop sequence via the engine thread (<= 0.6 s) + session summary
        if (g_impl->minimizedPause.exchange(false)) e.onResume();
        e.detach(reason);        // closes IF2 (<= 1.5 s)
        g_impl->apexIndex = -1;
        g_impl->apexJs = kNoJoystick;
        logLine("ARBS-W: 스트림 종료 처리 완료 - " + e.statusLine() + ", 트리거 rumble 무시 " +
                std::to_string(g_impl->triggerRumbleIgnored.exchange(0)) + "건");
    } catch (...) {
    }
}

void ArbsBridge::onWindowMinimized() {
    if (g_impl == nullptr || !arbsw::policy::kPauseOnMinimize || g_impl->apexJs == kNoJoystick) return;
    if (!g_impl->minimizedPause.exchange(true)) {
        logLine("ARBS-W: 스트림 창 최소화 -> APEX 정지(잠정 결정)");
        g_impl->engine->onPause();
    }
}

void ArbsBridge::onWindowRestored() {
    if (g_impl == nullptr || !arbsw::policy::kPauseOnMinimize) return;
    if (g_impl->minimizedPause.exchange(false)) {
        logLine("ARBS-W: 스트림 창 복원 -> APEX 재개");
        g_impl->engine->onResume();
    }
}

void ArbsBridge::onPcm(uint16_t controllerNumber, uint32_t sequence, const uint8_t* pcm, uint16_t frames) {
    if (g_impl == nullptr) return;
    try {
        // Borrowed LE bytes -> aligned int16 copy (x64 is little endian). <= 240 frames (ML_HAPTICS_MAX_FRAMES).
        int16_t buf[2 * 240];
        int n = frames > 240 ? 240 : frames;
        if (pcm == nullptr) n = 0;
        if (n > 0) std::memcpy(buf, pcm, static_cast<size_t>(n) * 4);
        g_impl->engine->onPcm(controllerNumber, sequence, n > 0 ? buf : nullptr, n);
    } catch (...) {
    }
}

bool ArbsBridge::consumeRumble(uint16_t controllerNumber, uint16_t lowFreq, uint16_t highFreq) {
    if (g_impl == nullptr) return false;
    const int idx = g_impl->apexIndex;
    if (idx < 0 || controllerNumber != idx) return false;
    try {
        if (!arbsw::policy::kRumbleCoreOnly && !g_impl->engine->isReady()) return false;  // [잠정 1] alternative
        g_impl->engine->onRumble(lowFreq, highFreq);
    } catch (...) {
    }
    return true;
}

bool ArbsBridge::consumeRumbleTriggers(uint16_t controllerNumber, uint16_t, uint16_t) {
    if (g_impl == nullptr) return false;
    const int idx = g_impl->apexIndex;
    if (idx < 0 || controllerNumber != idx) return false;
    // ARBS-A ApexController.rumbleTriggers: logged only (trigger motors are driven by the adaptive trigger path).
    if (g_impl->triggerRumbleIgnored.fetch_add(1) == 0) logLine("ARBS-W: 트리거 rumble 수신 - APEX 는 기록만(ARBS-A 와 같음)");
    return true;
}

bool ArbsBridge::consumeAdaptiveTriggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight,
                                         const uint8_t* left, const uint8_t* right) {
    if (g_impl == nullptr) return false;
    const int idx = g_impl->apexIndex;
    if (idx < 0 || controllerNumber != idx) return false;
    try {
        g_impl->engine->onTriggerEffects(controllerNumber, eventFlags, typeLeft, typeRight, left, right);
    } catch (...) {
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ gamepad.cpp
bool ArbsBridge::onGamepadAdded(int playerIndex, int joystickId, uint16_t sdlVid, uint16_t sdlPid) {
    if (g_impl == nullptr || !arbsw::needsIf2Lookup(sdlVid, sdlPid)) return false;
    try {
        std::string diag;
        const std::vector<arbsw::HidCandidate> list = arbsw::enumerateApexIf2(&diag);
        const arbsw::If2Pick pick = arbsw::pickApexIf2(list);
        const int fallbackPads = countFallbackPads();
        const arbsw::ApexMatch m = arbsw::identifyApex(sdlVid, sdlPid, pick.count, fallbackPads);
        logLine(arbs::jc::fmt("ARBS-W: 패드 번호 %d VID/PID %04x/%04x -> %s (IF2 %d개, XInput 대체 VID/PID 패드 %d개; %s)", playerIndex, sdlVid,
                              sdlPid, arbsw::apexMatchName(m), pick.count, fallbackPads, diag.c_str()));
        if (m == arbsw::ApexMatch::None) return false;
        if (g_impl->apexJs != kNoJoystick) {
            logLine("ARBS-W: APEX 가 이미 연결되어 있음 - 이 패드는 일반 패드로 처리");
            return false;
        }
        std::string why;
        std::unique_ptr<arbsw::WinHidPort> port = arbsw::WinHidPort::open(list[static_cast<size_t>(pick.index)], *g_impl->log, &why);
        if (!port) {
            logLine("ARBS-W: " + why + " -> 일반 패드로 처리(PCM 햅틱 광고 안 함)");
            return false;
        }
        g_impl->engine->attach(std::move(port));
        g_impl->engine->setControllerNumber(playerIndex);
        g_impl->apexIndex = playerIndex;
        g_impl->apexJs = joystickId;
        g_impl->leftTrigger = g_impl->rightTrigger = 0;
        return true;
    } catch (...) {
        logLine("ARBS-W: APEX 확인 중 예외 - 일반 패드로 처리");
        return false;
    }
}

uint32_t ArbsBridge::apexCaps(uint32_t sdlCaps) { return arbsw::apexCaps(sdlCaps); }

uint8_t ArbsBridge::apexType() { return arbsw::kTypePs; }

void ArbsBridge::sendApexArrival(int playerIndex, uint16_t activeGamepadMask, uint8_t type, uint32_t supportedButtons, uint32_t capabilities) {
    if (g_impl == nullptr) {
        LiSendControllerArrivalEvent(static_cast<uint8_t>(playerIndex), activeGamepadMask, type, supportedButtons, static_cast<uint16_t>(capabilities));
        return;
    }
    {
        std::lock_guard<std::mutex> g(g_impl->pendingMu);
        g_impl->pending = PendingArrival{true, playerIndex, activeGamepadMask, type, supportedButtons, capabilities};
    }
    switch (g_impl->gate.onApexAdded()) {
    case arbsw::ArrivalGate::Action::SendNow: {
        {
            std::lock_guard<std::mutex> g(g_impl->pendingMu);
            g_impl->pending.valid = false;
        }
        const int rc = LiSendControllerArrivalEvent(static_cast<uint8_t>(playerIndex), activeGamepadMask, type, supportedButtons,
                                                    static_cast<uint16_t>(capabilities));
        logLine("ARBS-W: APEX 도착 보고 번호 " + std::to_string(playerIndex) + " 타입 " + std::to_string(type) + "(PS) caps 0x" +
                arbs::jc::fmt("%x", capabilities) + " rc=" + std::to_string(rc) + (rc == 0 ? "" : " (0 이 아니면 실패: -2 = 입력 스트림 미초기화)"));
        break;
    }
    case arbsw::ArrivalGate::Action::Deferred:
        logLine("ARBS-W: APEX 도착 보류 - 스트림 연결 전(연결되면 1회 보고)");
        break;
    case arbsw::ArrivalGate::Action::AlreadySent: {
        std::lock_guard<std::mutex> g(g_impl->pendingMu);
        g_impl->pending.valid = false;
        logLine("ARBS-W: APEX 도착은 이미 보고됨 - 생략");
        break;
    }
    }
}

void ArbsBridge::onGamepadRemoved(int joystickId) {
    if (g_impl == nullptr || joystickId == kNoJoystick || g_impl->apexJs != joystickId) return;
    try {
        logLine("ARBS-W: APEX 패드 제거(SDL) -> 엔진 분리");
        g_impl->engine->detach("SDL 제거");
        g_impl->apexIndex = -1;
        g_impl->apexJs = kNoJoystick;
        g_impl->gate.onApexRemoved();
        std::lock_guard<std::mutex> g(g_impl->pendingMu);
        g_impl->pending.valid = false;
    } catch (...) {
    }
}

bool ArbsBridge::isApexJoystick(int joystickId) {
    return g_impl != nullptr && joystickId != kNoJoystick && g_impl->apexJs == joystickId;
}

void ArbsBridge::onApexInput() {
    if (g_impl == nullptr) return;
    g_impl->engine->onInputSeen();
}

void ArbsBridge::onApexTriggerAxis(bool left, int sdlAxisValue) {
    if (g_impl == nullptr) return;
    const int v = arbsw::triggerAxisTo255(sdlAxisValue);
    if (left) g_impl->leftTrigger = v;
    else g_impl->rightTrigger = v;
    g_impl->engine->onTriggerPosition(g_impl->leftTrigger, g_impl->rightTrigger);
}

// ------------------------------------------------------------------------------------------------ keyboard.cpp
void ArbsBridge::toggleUserStop() {
    if (g_impl == nullptr) return;
    try {
        arbs::Engine& e = *g_impl->engine;
        if (e.isUserStopped()) {
            e.userResume();
            logLine("ARBS-W: 단축키 Ctrl+Alt+Shift+H - APEX 재개");
        } else {
            logLine("ARBS-W: 단축키 Ctrl+Alt+Shift+H - APEX 정지");
            e.userStop();
        }
    } catch (...) {
    }
}

void ArbsBridge::reconnect() {
    if (g_impl == nullptr) return;
    try {
        const std::string r = g_impl->engine->manualRetry();
        logLine("ARBS-W: 단축키 Ctrl+Alt+Shift+J - APEX 다시 연결: " + r);
    } catch (...) {
    }
}

// ------------------------------------------------------------------------------------------------ settings page
QString ArbsBridge::statusText() {
    if (g_impl == nullptr) return QStringLiteral("NONE");
    return QString::fromStdString(g_impl->engine->statusText());
}

QString ArbsBridge::logDir() {
    if (g_impl == nullptr) return QString();
    return g_impl->logDir;
}
