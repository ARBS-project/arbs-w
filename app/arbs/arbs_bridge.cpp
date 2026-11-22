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
    arbsw::ApexBinder binder;               // main thread only (B2b 2-1)
    SDL_TimerID checkTimer = 0;             // main thread only
    std::atomic<SDL_TimerID> rttTimer{0};   // B2b 2-5
    std::atomic<bool> rttLogged{false};
    std::atomic<bool> verbose{false};
    arbsw::If2DetachWatch detach;     // B3 1-3 (log only, provisional decision D4)
    arbsw::PcmWait pcmWait;           // B3 1-4
    int apexArrivalsThisSession = 0;  // main thread only (B3 1-4: first arrival = "접속 후", later = "재연결 후")
    arbsw::HidRecheckWatch hidRecheck;  // B3b F2-D3 (log only, provisional decision D-B3b-3)
};

// B3b F2-D3: the 5 s / 30 s HID re-check runs on the main thread. The SDL timer thread posts the existing
// ARBS_SDL_CODE_APEX_CHECK event with a negative "generation" (= -checkpoint ms); binder generations are >= 1, so
// onApexCheck can tell them apart without a new event code (session.cpp / gamepad.cpp unchanged).
constexpr int kHidRecheckFirstCode = -static_cast<int>(arbsw::HidRecheckWatch::kFirstMs);
constexpr int kHidRecheckSecondCode = -static_cast<int>(arbsw::HidRecheckWatch::kSecondMs);

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

int64_t nowMs() { return g_impl->clock.nowNs() / 1000000LL; }

// ---- B2b 2-5: Moonlight RTT -> Engine::onMoonlightRtt every second (session summary "Moonlight RTT 평균") ----
Uint32 rttTimerCallback(Uint32 interval, void*) {
    Impl* im = g_impl;
    if (im == nullptr || !im->engine) return 0;
    try {
        // B3 1-3 [잠정 D4]: IF2 detached >= 5 s and SDL has not removed the APEX -> one notice (no release in b2d)
        if (im->detach.onTick(im->clock.nowNs())) logLine(arbsw::detachNoticeLine());
        // B3b F2-D3 [잠정 D-B3b-3]: checkpoint due -> main thread does the SDL check + HID list (one event each)
        const int64_t checkpoint = im->hidRecheck.onTick(im->clock.nowNs());
        if (checkpoint > 0) {
            SDL_Event ev = {};
            ev.type = SDL_USEREVENT;
            ev.user.code = ARBS_SDL_CODE_APEX_CHECK;
            ev.user.data1 = reinterpret_cast<void*>(static_cast<intptr_t>(-checkpoint));
            SDL_PushEvent(&ev);
        }
        uint32_t rtt = 0, var = 0;
        // common-c: try-lock on the ENet mutex + cached values; false until the first RTT estimate exists.
        if (LiGetEstimatedRttInfo(&rtt, &var)) {
            im->engine->onMoonlightRtt(static_cast<int>(rtt));
            if (!im->rttLogged.exchange(true)) {
                logLine(arbs::jc::fmt("ARBS-W: Moonlight RTT 첫 표본 %ums(분산 %ums) - 1초마다 세션 요약 'Moonlight RTT 평균'에 반영",
                                      rtt, var));
            } else if (im->verbose && im->log && im->log->perSecondAllowed()) {
                logLine(arbs::jc::fmt("Moonlight 통계: 네트워크RTT=%ums 분산=%ums", rtt, var));
            }
        }
    } catch (...) {
    }
    return interval;
}

void startRttTimer() {
    if (g_impl->rttTimer.load() != 0) return;
    g_impl->rttLogged = false;
    g_impl->rttTimer = SDL_AddTimer(1000, rttTimerCallback, nullptr);
}

// Before onSessionEnd, so the last summary has every sample and no sample arrives after the session (K10).
void stopRttTimer() {
    const SDL_TimerID t = g_impl->rttTimer.exchange(0);
    if (t != 0) SDL_RemoveTimer(t);
}

// ---- B2b 2-1: delayed presumed check ----
Uint32 checkTimerCallback(Uint32, void* param) {
    // SDL timer thread: only post an event; the binder runs on the main thread.
    SDL_Event ev = {};
    ev.type = SDL_USEREVENT;
    ev.user.code = ARBS_SDL_CODE_APEX_CHECK;
    ev.user.data1 = param;
    SDL_PushEvent(&ev);
    return 0;
}

void scheduleCheckTimer(int64_t atMs) {
    int64_t delay = atMs - nowMs();
    if (delay < 1) delay = 1;
    if (g_impl->checkTimer != 0) SDL_RemoveTimer(g_impl->checkTimer);
    g_impl->checkTimer = SDL_AddTimer(static_cast<Uint32>(delay), checkTimerCallback,
                                      reinterpret_cast<void*>(static_cast<intptr_t>(g_impl->binder.generation())));
}

void stopCheckTimer() {
    if (g_impl->checkTimer != 0) SDL_RemoveTimer(g_impl->checkTimer);
    g_impl->checkTimer = 0;
}

bool sdlHasExactDevice() {
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        if (arbsw::isExactVidPid(SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i))) return true;
    }
    return false;
}

struct If2Scan {
    bool done = false;
    std::vector<arbsw::HidCandidate> list;
    arbsw::If2Pick pick;
    std::string diag;
    int count() const { return done ? pick.count : -1; }
    std::string text() const { return done ? arbs::jc::fmt(" (IF2 %d개; %s)", pick.count, diag.c_str()) : std::string(); }
};

void scanIf2(If2Scan& s) {
    if (s.done) return;
    s.list = arbsw::enumerateApexIf2(&s.diag);
    s.pick = arbsw::pickApexIf2(s.list);
    s.done = true;
}

// B3 1-4: the APEX arrival went out (rc = 0) -> start waiting for the first PCM of that number.
void armPcmWait(int index) {
    const arbsw::PcmWait::Kind kind = arbsw::PcmWait::kindFor(g_impl->apexArrivalsThisSession++);
    g_impl->pcmWait.arm(g_impl->clock.nowNs(), index, kind);
}

// B3 1-4: unbind / session end before the first PCM -> "... 첫 PCM 없음(N ms 기다림, ...)".
void cancelPcmWait(const char* reason) {
    arbsw::PcmWait::Result r;
    if (g_impl->pcmWait.cancel(g_impl->clock.nowNs(), &r)) logLine(arbsw::pcmWaitNoneLine(r, reason));
}

// Stop sequence (engine thread, <= 1.5 s) and IF2 close; bridge state back to "no APEX".
void unbindEngine(const char* reason) {
    cancelPcmWait(reason);
    g_impl->detach.disarm();
    const int64_t t0 = g_impl->clock.nowNs();
    g_impl->engine->detach(reason);
    g_impl->apexIndex = -1;
    g_impl->apexJs = kNoJoystick;
    g_impl->leftTrigger = g_impl->rightTrigger = 0;
    g_impl->gate.onApexRemoved();
    {
        std::lock_guard<std::mutex> g(g_impl->pendingMu);
        g_impl->pending.valid = false;
    }
    logLine(arbs::jc::fmt("ARBS-W: 엔진 분리 완료(%s, %lldms)", reason,
                          static_cast<long long>((g_impl->clock.nowNs() - t0) / 1000000LL)));
    // B3b F2-D2 [잠정 D-B3b-3]: an engine thread that missed the 1.5 s join keeps its IF2 handle (F2 hypothesis H1)
    logLine(arbsw::unbindPortsLine(arbsw::WinHidPort::openCount()));
}

// B3b F2-D3 [잠정 D-B3b-3] (main thread): [checkpointMs] after an IF2 detach. SDL still lists a 37d7/2502 device ->
// no line (not "unseen"). Otherwise one Windows HID list summary (enumerateApexIf2: access-0 open + close per
// pre-filtered APEX path only, same as every bind) -> tells a Windows-level miss from an SDL-level miss.
void hidRecheck(int64_t checkpointMs) {
    if (sdlHasExactDevice()) return;
    std::string diag;
    int donglePaths = -1;  // B4 [잠정 D-B4-4]: 0 = dongle not in Windows (pulled) -> info wording
    arbsw::enumerateApexIf2(&diag, &donglePaths);
    logLine(arbsw::hidRecheckLine(checkpointMs, diag, arbsw::WinHidPort::openCount(), donglePaths));
}

void logJoystickList();

// IF2 open + attach + controller number + trigger/input seeds (B2b 2-3).
bool bindTo(int js, int index, arbsw::ApexMatch kind, If2Scan& s) {
    const int64_t bindT0 = g_impl->clock.nowNs();  // B3 1-2: main-thread time of this bind
    const auto msSince = [](int64_t t) { return (g_impl->clock.nowNs() - t) / 1000000LL; };
    // B2c 2-2: SDL joystick list again at every bind (after a dongle replug it shows whether SDL HIDAPI took IF2)
    logJoystickList();
    scanIf2(s);
    if (s.pick.count != 1 || s.pick.index < 0) {
        logLine("ARBS-W: APEX 연결 취소 - IF2 " + std::to_string(s.pick.count) + "개(정확히 1개일 때만) -> 일반 패드");
        g_impl->binder.onBindFailed(js);
        return false;
    }
    // B2c 2-2 (analysis gap 1): the old port must be gone before the new open. Core attach() releases the previous port
    // only after we have opened the new one, and after a detach that took > 1.5 s Core still holds it. Release it here
    // first. (Core's own order - port released before the late engine thread is joined - is analysis gap 2, record only.)
    const int before = arbsw::WinHidPort::openCount();
    const int64_t cleanupT0 = g_impl->clock.nowNs();
    if (before > 0) {
        logLine(arbs::jc::fmt("ARBS-W: 이 프로세스에 이전 IF2 포트 %d개가 아직 열려 있음 - 새로 열기 전에 엔진 분리로 닫음", before));
        g_impl->engine->detach("새 IF2 열기 전 이전 포트 정리");
        logLine(arbs::jc::fmt("ARBS-W: 이전 IF2 포트 정리 후 열린 포트 %d개", arbsw::WinHidPort::openCount()));
    }
    const int64_t cleanupMs = msSince(cleanupT0);
    std::string why;
    const int64_t openT0 = g_impl->clock.nowNs();
    std::unique_ptr<arbsw::WinHidPort> port = arbsw::WinHidPort::open(s.list[static_cast<size_t>(s.pick.index)], *g_impl->log, &why);
    const int64_t openMs = msSince(openT0);
    if (!port) {
        logLine("ARBS-W: " + why + " -> 일반 패드로 처리(PCM 햅틱 광고 안 함)");
        logLine(arbsw::bindStallLine(msSince(bindT0), cleanupMs, openMs));
        g_impl->binder.onBindFailed(js);
        return false;
    }
    // B3 1-3: the port reports its detach (ReadFile error 995 etc.) once; stale generations are ignored.
    const int detachGen = g_impl->detach.arm();
    g_impl->hidRecheck.stop();  // B3b F2-D3: the dongle is back and bound -> no re-check for the old detach
    port->setDisconnectListener([detachGen] {
        if (g_impl == nullptr) return;
        const int64_t now = g_impl->clock.nowNs();
        // B3b F2-D3: the first detach of the current port starts the 5 s / 30 s re-checks (stale ports do not)
        if (g_impl->detach.onDetach(detachGen, now)) g_impl->hidRecheck.start(now);
    });
    g_impl->engine->attach(std::move(port));
    g_impl->engine->setControllerNumber(index);
    g_impl->apexIndex = index;
    g_impl->apexJs = js;
    // Seed: current L2/R2 of the bound pad (already pressed at bind time -> right position), same 0..255 unit as the
    // ARBS-A IF0 trigger byte (B2b analysis 2-4).
    uint16_t vid = 0, pid = 0;
    int rawL = 0, rawR = 0;
    SDL_GameController* gc = SDL_GameControllerFromInstanceID(static_cast<SDL_JoystickID>(js));
    if (gc != nullptr) {
        vid = SDL_GameControllerGetVendor(gc);
        pid = SDL_GameControllerGetProduct(gc);
        rawL = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        rawR = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    }
    g_impl->leftTrigger = arbsw::triggerAxisTo255(rawL);
    g_impl->rightTrigger = arbsw::triggerAxisTo255(rawR);
    g_impl->engine->onTriggerPosition(g_impl->leftTrigger, g_impl->rightTrigger);
    const bool exact = kind == arbsw::ApexMatch::Exact;
    // An SDL XInput device with the APEX VID/PID = the pad is on the dongle: count it as the first input (no 3 s wait).
    if (exact) g_impl->engine->onInputSeen();
    logLine(arbs::jc::fmt("ARBS-W: APEX 연결 - 패드 번호 %d VID/PID %04x/%04x (%s), 트리거 시작값 L=%d R=%d", index, vid, pid,
                          exact ? "VID/PID 일치" : "추정 매칭", g_impl->leftTrigger, g_impl->rightTrigger));
    logLine(std::string("ARBS-W: Windows 안내 - Core 로그의 'IF0 입력 리포트'는 Windows 에서 APEX 패드의 SDL 버튼·축 입력을 뜻함"
                        "(트리거 위치 = SDL L2/R2 0..32767 -> 0..255). ") +
            (exact ? "VID/PID 일치 연결은 바로 입력 수신으로 처리(대기 없이 GetInfo)"
                   : "추정 연결은 APEX 를 만지면 대기가 끝남('입력 리포트 없이 대기 만료'가 나와도 정상 - 3초 뒤 GetInfo 진행)"));
    logLine(arbsw::bindStallLine(msSince(bindT0), cleanupMs, openMs));
    return true;
}

ArbsBind execute(const arbsw::ApexBinder::Decision& d, If2Scan& s) {
    using A = arbsw::ApexBinder::Action;
    ArbsBind r;
    switch (d.action) {
    case A::None:
        break;
    case A::BindExact:
    case A::BindPresumed:
        if (bindTo(d.bindJs, d.bindIndex, d.bindKind, s)) r.boundJs = d.bindJs;
        break;
    case A::Rebind:
        logLine(arbs::jc::fmt("ARBS-W: APEX 다시 묶기 - 추정 연결 패드(번호 %d) -> VID/PID 일치 패드(번호 %d): 정지 시퀀스 후 재도착 보고",
                              d.unbindIndex, d.bindIndex));
        unbindEngine("APEX 다시 묶기");
        r.unboundJs = d.unbindJs;
        if (bindTo(d.bindJs, d.bindIndex, d.bindKind, s)) r.boundJs = d.bindJs;
        break;
    case A::Unbind:
    case A::UnbindThenBindExact: {
        // B3 1-3: how long SDL kept the APEX after IF2 was detached (EA-B2c-R-02: 1..24 s)
        const int64_t sinceDetach = g_impl->detach.onSdlRemoved(g_impl->clock.nowNs());
        if (sinceDetach >= 0) logLine(arbsw::detachRemovedLine(sinceDetach));
        logLine("ARBS-W: APEX 패드 제거(SDL) -> 엔진 분리");
        unbindEngine("SDL 제거");
        if (d.action == A::UnbindThenBindExact && bindTo(d.bindJs, d.bindIndex, d.bindKind, s)) r.boundJs = d.bindJs;
        break;
    }
    }
    if (d.checkAtMs >= 0) scheduleCheckTimer(d.checkAtMs);
    return r;
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
    if (rc == 0) armPcmWait(a.index);  // B3 1-4
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

int ArbsBridge::dumpPrefs() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(sizeof(exe) / sizeof(exe[0])));
    if (n == 0 || n >= sizeof(exe) / sizeof(exe[0])) return 4;
    std::wstring dir(exe, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 4;
    dir.resize(slash);
    // Portable only: without portable.dat, QSettings would read HKCU -> do not touch it.
    if (GetFileAttributesW((dir + L"\\portable.dat").c_str()) == INVALID_FILE_ATTRIBUTES) return 3;
    const StreamingPreferences* prefs = StreamingPreferences::get();  // reload() only - save() is never called here
    const std::string body = arbsw::prefsDumpText(prefs->arbsSource, arbsw::editionName());
    QFile f(QString::fromStdWString(dir + L"\\arbs-prefs-dump.txt"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 4;
    const qint64 w = f.write(body.data(), static_cast<qint64>(body.size()));
    f.close();
    return w == static_cast<qint64>(body.size()) ? 0 : 4;
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

    // B4 [잠정 D-B4-3]: edition from the build wrapper (was the fixed text "B2d")
    logLine(arbsw::logHeaderLine(arbsw::editionName(), VERSION_STR));
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
        stopRttTimer();
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
        // B4 [잠정 D-B4-5]: new stream session -> the error-32 warning may be written once again. The input handler (and
        // so every APEX bind / IF2 open of this session) is created after this call (session.cpp).
        arbsw::WinHidPort::resetSessionNotices();
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
        g_impl->verbose = s.verbose;
        startRttTimer();
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
        stopRttTimer();
        stopCheckTimer();
        g_impl->binder.reset();  // pads are closed with the input handler; stale check events are ignored (generation)
        g_impl->gate.reset();
        {
            std::lock_guard<std::mutex> g(g_impl->pendingMu);
            g_impl->pending.valid = false;
        }
        cancelPcmWait(reason);  // B3 1-4
        g_impl->apexArrivalsThisSession = 0;
        g_impl->detach.disarm();  // B3 1-3
        g_impl->hidRecheck.stop();  // B3b F2-D3
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
        // B3 1-4: first PCM packet for the armed number after the APEX arrival (atomic fast path when not armed)
        arbsw::PcmWait::Result r;
        if (g_impl->pcmWait.onPcm(g_impl->clock.nowNs(), controllerNumber, &r)) logLine(arbsw::pcmWaitLine(r));
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
ArbsBind ArbsBridge::onGamepadAdded(int playerIndex, int joystickId, uint16_t sdlVid, uint16_t sdlPid) {
    if (g_impl == nullptr) return ArbsBind();
    try {
        If2Scan s;
        if (arbsw::isExactVidPid(sdlVid, sdlPid)) scanIf2(s);  // only 37D7/2502 needs HID enumeration at arrival
        const arbsw::ApexBinder::Decision d =
            g_impl->binder.onPadAdded(joystickId, playerIndex, sdlVid, sdlPid, nowMs(), s.count(), sdlHasExactDevice());
        logLine(arbs::jc::fmt("ARBS-W: 패드 번호 %d VID/PID %04x/%04x -> %s%s", playerIndex, sdlVid, sdlPid, d.why.c_str(),
                              s.text().c_str()));
        return execute(d, s);
    } catch (...) {
        logLine("ARBS-W: APEX 확인 중 예외 - 일반 패드로 처리");
        return ArbsBind();
    }
}

ArbsBind ArbsBridge::onApexCheck(int generation) {
    if (g_impl == nullptr) return ArbsBind();
    if (generation == kHidRecheckFirstCode || generation == kHidRecheckSecondCode) {  // B3b F2-D3 (not a binder check)
        try {
            hidRecheck(-static_cast<int64_t>(generation));
        } catch (...) {
        }
        return ArbsBind();
    }
    if (generation != g_impl->binder.generation()) return ArbsBind();
    try {
        g_impl->checkTimer = 0;  // one-shot timer has fired
        If2Scan s;
        if (g_impl->binder.checkNeedsIf2()) scanIf2(s);
        const arbsw::ApexBinder::Decision d = g_impl->binder.onCheck(nowMs(), s.count(), sdlHasExactDevice());
        logLine("ARBS-W: APEX 추정 확인(첫 패드 도착 1000ms 뒤) -> " + d.why + s.text());
        return execute(d, s);
    } catch (...) {
        logLine("ARBS-W: APEX 추정 확인 중 예외 - 연결 안 함");
        return ArbsBind();
    }
}

void ArbsBridge::onGamepadIndexChanged(int joystickId, int playerIndex) {
    if (g_impl == nullptr) return;
    g_impl->binder.onIndexChanged(joystickId, playerIndex);
    if (g_impl->apexJs == joystickId) {
        const int old = g_impl->apexIndex.exchange(playerIndex);
        g_impl->engine->setControllerNumber(playerIndex);
        // B2c 2-3 (verify B2b question #6): the APEX arrival for the new number follows this call; without this the gate
        // would drop it as "already sent" and the host would never create the new number as PS/PCM.
        if (arbsw::reopenGateOnIndexChange(g_impl->gate, old, playerIndex)) {
            {
                std::lock_guard<std::mutex> g(g_impl->pendingMu);
                if (g_impl->pending.valid) g_impl->pending.index = playerIndex;
            }
            logLine(arbs::jc::fmt("ARBS-W: APEX 번호 %d -> %d 변경 - 도착 게이트 다시 열기(새 번호로 도착 보고)", old, playerIndex));
        }
    }
}

bool ArbsBridge::isExactPad(uint16_t sdlVid, uint16_t sdlPid) { return arbsw::isExactVidPid(sdlVid, sdlPid); }

bool ArbsBridge::sdlListHasExact() { return sdlHasExactDevice(); }

bool ArbsBridge::apexBound() { return g_impl != nullptr && g_impl->apexJs != kNoJoystick; }

bool ArbsBridge::exactPending() {
    if (g_impl == nullptr) return false;
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; i++) {
        if (!arbsw::isExactVidPid(SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i))) continue;
        // only game controllers get a DEVICEADDED (gamepad.cpp); a plain joystick would stay "pending" forever
        if (!SDL_IsGameController(i)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        if (id >= 0 && !g_impl->binder.seen(static_cast<int>(id))) return true;
    }
    return false;
}

bool ArbsBridge::onArrivalDropped(int joystickId, uint16_t sdlVid, uint16_t sdlPid, const char* why) {  // B5 D-B5-4
    if (g_impl == nullptr) return false;
    if (!g_impl->binder.markSeen(joystickId, sdlVid, sdlPid)) return false;
    logLine(arbs::jc::fmt("ARBS-W: 37d7/2502 도착 처리 못 함(%s) - 숨김 보류 해제", why != nullptr ? why : "?"));
    return true;
}

void ArbsBridge::log(const char* line) {
    if (line != nullptr) logLine(line);
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
        if (rc == 0) armPcmWait(playerIndex);  // B3 1-4
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

ArbsBind ArbsBridge::onGamepadRemoved(int joystickId) {
    if (g_impl == nullptr || joystickId == kNoJoystick) return ArbsBind();
    try {
        const int index = g_impl->binder.indexOf(joystickId);
        If2Scan s;
        if (g_impl->binder.removalNeedsIf2(joystickId)) scanIf2(s);
        const arbsw::ApexBinder::Decision d = g_impl->binder.onPadRemoved(joystickId, nowMs(), s.count());
        if (index >= 0) logLine(arbs::jc::fmt("ARBS-W: 패드 번호 %d 제거(SDL) -> %s%s", index, d.why.c_str(), s.text().c_str()));
        return execute(d, s);
    } catch (...) {
        return ArbsBind();
    }
}

bool ArbsBridge::isApexJoystick(int joystickId) {
    return g_impl != nullptr && joystickId != kNoJoystick && g_impl->apexJs == joystickId;
}

void ArbsBridge::onApexInput() {
    if (g_impl == nullptr) return;
    g_impl->engine->onInputSeen();
    // B3 1-3: the APEX keeps being used while IF2 is gone -> one warning per detach (atomic fast path otherwise)
    if (g_impl->detach.onApexInput(g_impl->clock.nowNs())) logLine(arbsw::detachInputWarnLine());
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
