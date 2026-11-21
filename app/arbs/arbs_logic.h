// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W pure logic (B2 D7/D8): matching rules, frame conversion, caps, gates and small helpers that the Windows I/O
// layer (win_hid_port.cpp) and the Moonlight glue (arbs_bridge.cpp) use. No Qt, no SDL, no Win32 here, so every rule
// is unit-tested without touching a device (app/arbs/tests). Design: docs/analysis/B2_DESIGN.md sections 4-3..4-6.
#pragma once

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "arbs/engine.h"
#include "arbs/platform.h"
#include "arbs_numbering.h"
#include "arbs_settings.h"

namespace arbsw {

// ---- device identity (ASB src/flydigi/Apex6Protocol.h:11-18, Apex5Device.cpp:227-228; SDL3 usb_ids.h:97) ----
constexpr uint16_t kApexVid = 0x37D7;
constexpr uint16_t kApexDonglePid = 0x2502;
constexpr uint16_t kIf2UsagePage = 0xFFA0;
// SDL3 XInput driver fallback when XInputGetCapabilitiesEx fails (SDL_xinputjoystick.c:101-107).
constexpr uint16_t kXInputFallbackVid = 0x045E;
constexpr uint16_t kXInputFallbackPid = 0x028E;

// One HID interface found by enumeration (filled by win_hid_port.cpp; built by hand in tests).
struct HidCandidate {
    std::wstring path;
    uint16_t vid = 0;
    uint16_t pid = 0;
    uint16_t usagePage = 0;
    uint16_t usage = 0;
    uint16_t inLen = 0;   // InputReportByteLength (report ID byte included)
    uint16_t outLen = 0;  // OutputReportByteLength (report ID byte included)
};

// Cheap pre-filter on the interface path ("vid_37d7&pid_2502", case-insensitive) so that enumeration does not open
// unrelated HID devices at all.
bool pathMayBeApex(const std::wstring& path);
// VID 37D7, PID 2502, UsagePage 0xFFA0 (= dongle MI_02 / IF2).
bool isApexIf2(const HidCandidate& c);

struct If2Pick {
    int count = 0;    // number of matching IF2 interfaces
    int index = -1;   // index into the list when count == 1, otherwise -1 (0 = no dongle, 2+ = refuse)
};
If2Pick pickApexIf2(const std::vector<HidCandidate>& list);

// APEX identification at SDL DEVICEADDED (design 4-3).
enum class ApexMatch { None, Exact, Presumed };
const char* apexMatchName(ApexMatch m);
// [if2Count] = pickApexIf2().count, [fallbackPadCount] = number of SDL joysticks reporting 045E/028E right now.
//  Exact    : SDL VID/PID == 37D7/2502 and exactly one IF2.
//  Presumed : SDL gives the XInput fallback 045E/028E, exactly one IF2 and exactly one such pad.
//  None     : anything else (no IF2, two dongles, several fallback pads, other VID/PID) -> normal SDL path.
ApexMatch identifyApex(uint16_t sdlVid, uint16_t sdlPid, int if2Count, int fallbackPadCount);
// Only these VID/PIDs make the bridge enumerate HID at all.
bool needsIf2Lookup(uint16_t sdlVid, uint16_t sdlPid);
bool isExactVidPid(uint16_t sdlVid, uint16_t sdlPid);     // 37D7/2502
bool isFallbackVidPid(uint16_t sdlVid, uint16_t sdlPid);  // 045E/028E

// ---- B2b: which open SDL pad is the APEX (docs/analysis/B2b_APEX_ID.md section 5 2-1, 2-4) ----
// Main thread only. Rules:
//  1. 37D7/2502 + one IF2 -> Exact at once. Nothing bound -> BindExact; a Presumed pad bound -> Rebind to it;
//     an Exact pad bound -> None (one dongle).
//  2. 045E/028E is only a candidate: a check is scheduled at first-pad-arrival + kPresumeDelayMs.
//  3. At the check, Presumed only if: nothing bound, no open Exact pad, no 37D7/2502 anywhere in the SDL list (also
//     not-yet-opened devices), exactly one open 045E/028E pad, one IF2.
//  4. Bound pad removed -> Unbind; an open Exact pad (one IF2) is bound at once, otherwise rule 3 again (new check).
//  5. Any other pad removed -> list only, the engine is not touched.
//  B2b addition (beyond the analysis, reported to the planner): once SDL has shown a 37D7/2502 device in this session,
//  SDL can read the APEX VID/PID on this PC, so the XInput fallback presumption is not used for the rest of the session
//  (prevents the built-in 045E/028E pad of a handheld from being bound when the APEX is switched off).
class ApexBinder {
public:
    static constexpr int kNone = INT_MIN;
    static constexpr int64_t kPresumeDelayMs = 1000;
    enum class Action { None, BindExact, BindPresumed, Rebind, Unbind, UnbindThenBindExact };
    struct Decision {
        Action action = Action::None;
        int bindJs = kNone;        // pad to bind (BindExact / BindPresumed / Rebind / UnbindThenBindExact)
        int bindIndex = -1;
        ApexMatch bindKind = ApexMatch::None;
        int unbindJs = kNone;      // pad losing the binding (Rebind: still open; Unbind*: removed)
        int unbindIndex = -1;
        int64_t checkAtMs = -1;    // >= 0: call onCheck() at (or after) this time
        std::string why;           // log text for the decision
    };
    static const char* actionName(Action a);

    Decision onPadAdded(int js, int index, uint16_t vid, uint16_t pid, int64_t nowMs, int if2Count, bool sdlListHasExact);
    // [if2Count] is only needed when removalNeedsIf2(js) is true (pass -1 otherwise).
    Decision onPadRemoved(int js, int64_t nowMs, int if2Count);
    Decision onCheck(int64_t nowMs, int if2Count, bool sdlListHasExact);
    void onBindFailed(int js);               // IF2 could not be opened: nothing is bound
    void onIndexChanged(int js, int index);  // slot move in gamepad.cpp
    void reset();                            // session end: forget pads, new generation (stale checks are ignored)

    int boundJs() const { return boundJs_; }
    ApexMatch boundKind() const { return boundKind_; }
    int generation() const { return gen_; }
    bool checkPending() const { return checkPending_; }
    bool exactSeen() const { return exactSeen_; }
    bool removalNeedsIf2(int js) const;  // bound pad + an open Exact pad remains
    bool checkNeedsIf2() const;          // a check could bind (nothing bound, one candidate)
    size_t padCount() const { return pads_.size(); }
    int indexOf(int js) const;           // -1 if unknown
    // B2c 2-1: onPadAdded() was called for [js] in this session (bound or not). A 37d7/2502 game controller in the SDL
    // list that was never seen may still bind -> APEX-only mode keeps the other pads hidden until it is handled.
    bool seen(int js) const;

private:
    struct Pad {
        int js;
        int index;
        uint16_t vid, pid;
    };
    int64_t scheduleCheck(int64_t atMs);  // returns the time to report, or -1 if a check is already pending
    int openCount(bool exact) const;

    std::vector<Pad> pads_;
    std::vector<int> seen_;  // B2c: every joystick id handed to onPadAdded() since reset()
    int boundJs_ = kNone;
    ApexMatch boundKind_ = ApexMatch::None;
    int64_t firstAddMs_ = -1;
    int64_t checkAtMs_ = -1;
    bool checkPending_ = false;
    bool exactSeen_ = false;
    int gen_ = 1;
};

// ---- frames (ARBS Core frame = 32 bytes; Windows HID output report = report ID 0x00 + 32, ASB WHT:263-272) ----
constexpr size_t kFrameSize = 32;
constexpr size_t kWireSize = 33;
// OutputReportByteLength must hold report ID + 32 bytes. Larger is zero padded (ASB), smaller is refused.
bool outputLenUsable(uint16_t outLen);
// Writes [0x00][frame 32][zero padding up to outLen] into dst. Returns outLen, or 0 if outLen is unusable or dst small.
size_t buildOutputReport(const uint8_t* frame32, uint16_t outLen, uint8_t* dst, size_t dstCap);

// ---- arrival report (design 4-3; ARBS-A ApexController.java:67 -> 0x8003 core bits, OBSERVED(EA-A1-U-03)) ----
constexpr uint32_t kCapAnalogTriggers = 0x01;   // LI_CCAP_ANALOG_TRIGGERS
constexpr uint32_t kCapRumble = 0x02;           // LI_CCAP_RUMBLE
constexpr uint32_t kCapTriggerRumble = 0x04;    // LI_CCAP_TRIGGER_RUMBLE
constexpr uint32_t kCapHapticsPcm = 0x8000;     // LI_CCAP_HAPTICS_PCM (Nonary common-c ControllerHaptics.h)
constexpr uint8_t kTypePs = 0x02;               // LI_CTYPE_PS
// SDL-computed caps | analog triggers | rumble | PCM haptics, without trigger rumble.
uint32_t apexCaps(uint32_t sdlCaps);

// SDL trigger axis 0..32767 -> 0..255 with Java Math.round semantics (design 2-4 #11). Negative -> 0.
int triggerAxisTo255(int sdlAxisValue);

// ---- SDL hints (design 4-4) ----
constexpr const char* kIgnoreEntry = "0x37d7/0x2502";
// Appends [entry] to an existing comma list (environment / previous hint) unless it is already there.
std::string appendIgnoreDevice(const std::string& existing, const std::string& entry);
// True for an SDL joystick path that looks like the dongle IF2 (HID path with VID_37D7 and MI_02), i.e. an SDL HIDAPI
// driver has opened IF2 although it should have been ignored -> warning in the ARBS log.
bool pathLooksLikeIf2(const std::string& sdlPath);

// ---- logging ----
// Engine lines that are also copied into the Moonlight log (state changes / summaries only, never the 1 s lines).
bool isStateLine(const std::string& line);

// ---- Win32 helpers that are pure arithmetic ----
// Error codes from ReadFile/WriteFile/GetOverlappedResult that mean the dongle is gone (design 4-5):
// 1167 ERROR_DEVICE_NOT_CONNECTED, 433 ERROR_NO_SUCH_DEVICE, 2 FILE_NOT_FOUND, 6 INVALID_HANDLE, 995 OPERATION_ABORTED,
// 109 BROKEN_PIPE (pipe-based unit test stand-in). Anything else is a plain Error.
arbs::IoResult classifyIoError(unsigned long win32Error);
// SetWaitableTimer relative due time (100 ns units, negative). Rounds up so the wait never ends early; minimum -1.
int64_t waitableDueTime100ns(int64_t remainingNs);
// QueryPerformanceCounter ticks -> ns without overflow.
int64_t ticksToNs(int64_t ticks, int64_t freq);

// ---- input report ring (read thread -> engine thread), design 4-5 "링(64)" ----
class ReportRing {
public:
    static constexpr size_t kSlots = 64;
    static constexpr size_t kSlotBytes = 64;
    // Copies up to kSlotBytes. When full the oldest report is dropped and counted.
    void push(const uint8_t* data, size_t len);
    bool pop(uint8_t* out, size_t cap, size_t* outLen);
    size_t size() const { return count_; }
    uint64_t dropped() const { return dropped_; }
    void clear();

private:
    uint8_t buf_[kSlots][kSlotBytes] = {};
    size_t len_[kSlots] = {};
    size_t head_ = 0, count_ = 0;
    uint64_t dropped_ = 0;
};

// ---- arrival gate (design 4-3 "도착 시점 보장", ARBS-A A1-2b EA-A1-C-03) ----
// The APEX arrival is sent only while the stream is live, at most once per (session, attach).
class ArrivalGate {
public:
    enum class Action { SendNow, Deferred, AlreadySent };
    Action onApexAdded();            // main thread, at DEVICEADDED of the APEX
    bool setLive();                  // stream live; true = a deferred arrival must be sent now (and is marked sent)
    void onApexRemoved();            // DEVICEREMOVED: next add sends again
    void reset();                    // session end
    bool live() const;
    bool pending() const;

private:
    mutable std::mutex mu_;
    bool live_ = false, pending_ = false, sent_ = false;
};

// B2c 2-3 (verify B2b question #6): the bound APEX moved to another controller number (slot move) -> the arrival on the
// new number must not be dropped as "already sent". Re-opens [g] (onApexRemoved) when [oldIndex] >= 0 and the number
// really changes; returns true then. The ArrivalGate rules themselves are unchanged.
bool reopenGateOnIndexChange(ArrivalGate& g, int oldIndex, int newIndex);

// ---- settings -> Core engine config ----
arbs::Engine::Config toEngineConfig(const ArbsSettings& s);

// ---- B3 1-2 (ARBS-W B2d): main-thread stall lines (docs/analysis/B3.md 1-2, provisional decision D-B3-2) ----
// "ARBS-W: IF2 열기 - 메인 스레드 정지 약 N ms(배타 재시도 R회, 결과 배타|공유|실패)"
std::string openStallLine(int64_t stallMs, int retries, const char* result);
// "ARBS-W: APEX 묶기 메인 스레드 N ms(이전 포트 정리 C ms + IF2 열기 O ms)"
std::string bindStallLine(int64_t totalMs, int64_t cleanupMs, int64_t openMs);

// ---- B3 1-3 (ARBS-W B2d): IF2 detached (error 995 etc.) but SDL has not removed the APEX yet ----
// Provisional decision D4 (planner has not ruled): b2d only MEASURES and WARNS. The "soft release" after N = 5 s
// (unbind + show the hidden pads) is designed in docs/analysis/B3.md 1-3 but NOT implemented.
// Threads: arm/disarm/onSdlRemoved/onApexInput on the main thread, onDetach on the port reader or engine thread,
// onTick on the SDL timer thread (1 s RTT timer). All state is under one mutex; onApexInput has an atomic fast path.
class If2DetachWatch {
public:
    static constexpr int64_t kNoticeMs = 5000;     // [잠정 D4] N = 5 s
    static constexpr int64_t kInputWarnMs = 2000;  // APEX SDL input keeps coming > 2 s while IF2 is gone
    int arm();     // new bind: clears the state, returns the generation the port listener must pass to onDetach
    void disarm(); // engine unbound / session end
    // First detach of the current generation: records the time, returns true. Stale generation / again -> false.
    bool onDetach(int generation, int64_t nowNs);
    // True once per detach: >= kNoticeMs since the detach and no SDL removal yet.
    bool onTick(int64_t nowNs);
    // SDL removed the bound APEX: ms since the detach (>= 0), or -1 when IF2 was not detached. Disarms.
    int64_t onSdlRemoved(int64_t nowNs);
    // True once per detach: an APEX SDL input arrives more than kInputWarnMs after the first APEX input seen after the
    // detach (the pad is still in use although its vibration path is gone).
    bool onApexInput(int64_t nowNs);
    bool detached() const { return detachedFlag_.load(); }

private:
    mutable std::mutex mu_;
    std::atomic<bool> detachedFlag_{false};
    int gen_ = 0;
    bool armed_ = false, detached_ = false, noticed_ = false, warned_ = false;
    int64_t detachNs_ = 0, firstInputNs_ = -1;
};
// "ARBS-W: IF2 분리 후 SDL 제거까지 N ms"
std::string detachRemovedLine(int64_t ms);
// "ARBS-W: IF2 분리 5초 경과 - SDL 제거 아직 없음(...)"
std::string detachNoticeLine();
// "ARBS-W 경고: IF2 분리 상태인데 APEX 입력이 계속 들어옴 - 진동 경로 끊김(동글 다시 꽂기)"
std::string detachInputWarnLine();

// ---- B3 1-4 (ARBS-W B2d): time from the APEX arrival report (rc = 0) to the first PCM packet for that number ----
// Measured in the bridge (Core has no hook; Core is unchanged). Provisional decision D-B3-4: the start is the arrival
// report, the first arrival of a session is reported as "접속 후", later ones as "재연결 후".
// Threads: arm/cancel on the main thread, onPcm on the common-c receive thread (atomic fast path; the mutex is only
// taken while a wait is armed). One line per arm.
class PcmWait {
public:
    enum class Kind : uint8_t { AfterConnect, AfterReconnect };
    struct Result {
        int64_t ms = 0;
        int index = -1;
        Kind kind = Kind::AfterConnect;
    };
    static Kind kindFor(int previousArrivalsThisSession) { return previousArrivalsThisSession > 0 ? Kind::AfterReconnect : Kind::AfterConnect; }
    void arm(int64_t nowNs, int index, Kind kind);  // a second arm replaces the first
    bool onPcm(int64_t nowNs, int index, Result* out);  // true once per arm, only for the armed number
    bool cancel(int64_t nowNs, Result* out);            // true if a wait was armed (-> "없음" line)
    bool armed() const { return armed_.load(); }

private:
    std::atomic<bool> armed_{false};
    std::mutex mu_;
    int64_t startNs_ = 0;
    int index_ = -1;
    Kind kind_ = Kind::AfterConnect;
};
// "ARBS-W: 재연결 후 첫 PCM N ms(APEX 도착 보고 -> 첫 PCM, 번호 I)" / "ARBS-W: 접속 후 첫 PCM N ms(번호 I)"
std::string pcmWaitLine(const PcmWait::Result& r);
// "ARBS-W: 재연결 후 첫 PCM 없음(N ms 기다림, 번호 I, <reason>)" (접속 후 ... for the first arrival)
std::string pcmWaitNoneLine(const PcmWait::Result& r, const char* reason);

// ---- B3b F2 (ARBS-W b2e): diagnostics only, provisional decision D-B3b-3 (planner has not ruled) ----
// docs/analysis/B3b.md section 2: after an IF2 detach (995) the dongle was not seen again. b2e only LOGS (D1-D3); the
// mitigations m1 (Core: close the port when a late engine thread ends) and m2 (delayed IF2 open) wait for these logs.
// D1: APEX command byte of a 32-byte Core frame (5A A5 <cmd> ...): 0x01 GetInfo, 0x53 motor route, 0x57 realtime;
//     -1 when the magic does not match.
int apexFrameCommand(const uint8_t frame[32]);
// D1: "ARBS-W: IF2 열기 후 분리까지 N ms(GetInfo 송신 전|후 n회, 0x53 송신 전|후 n회)"
std::string openToDetachLine(int64_t ms, uint64_t getInfoWrites, uint64_t motorRouteWrites);
// D2: end of the engine unbind. 0 -> "ARBS-W: 엔진 분리 뒤 이 프로세스에 열린 IF2 포트 0개";
//     > 0 -> a warning (an engine thread that did not end in 1.5 s keeps its IF2 handle, F2 hypothesis H1).
std::string unbindPortsLine(int openPorts);
// D3: 5 s and 30 s after an IF2 detach, once each, while SDL has no 37d7/2502 device: one Windows HID list summary.
// Threads: start on the port reader/engine thread (detach), stop on the main thread (new bind / session end), onTick on
// the SDL timer thread. One mutex.
class HidRecheckWatch {
public:
    static constexpr int64_t kFirstMs = 5000;
    static constexpr int64_t kSecondMs = 30000;
    void start(int64_t nowNs);  // a later detach restarts both checkpoints
    void stop();
    // Due checkpoint: kFirstMs or kSecondMs (each once per start), 0 = nothing due. If both are due on the same tick
    // (timer stalled > 30 s) only kSecondMs is returned (one HID scan).
    int64_t onTick(int64_t nowNs);
    bool active() const;

private:
    mutable std::mutex mu_;
    bool active_ = false, firstDone_ = false, secondDone_ = false;
    int64_t startNs_ = 0;
};
// D3: "ARBS-W: IF2 분리 S초 뒤 SDL 에 37d7/2502 없음 - Windows <hid summary>, 이 프로세스 IF2 포트 K개(F2 진단)"
// B4 [잠정 D-B4-4] (ARBS-W b2f): [donglePaths] = APEX dongle paths Windows still lists (enumerateApexIf2). 0 -> the dongle
// is not in Windows at all (a pulled dongle, normal) -> an info line instead of the diagnosis wording:
// "ARBS-W: IF2 분리 S초 뒤 - 동글이 Windows 에 없음(뽑힘이면 정상, 정보) - <hid summary>, 이 프로세스 IF2 포트 K개".
// >= 1 (or unknown, -1) -> the b2e line above unchanged (dongle present but SDL does not list it).
std::string hidRecheckLine(int64_t checkpointMs, const std::string& hidSummary, int ownPorts, int donglePaths = -1);

// ---- B4 (ARBS-W b2f) [잠정 D-B4-3]: edition name in the first log line ----
// The build wrapper (client-w/scripts/arbs-w-build.bat) derives ARBS_W_EDITION from the output zip name
// (arbs-w-b2f-portable.zip -> b2f); arbs.pri turns it into ARBS_W_EDITION_STR. Builds without it (unit tests, a plain
// qmake build) say "dev".
const char* editionName();
// "ARBS-W <edition> 로그 - Moonlight <version> + ARBS Core (APEX 햅틱). IP·토큰·PIN 은 기록하지 않음"
std::string logHeaderLine(const char* edition, const char* moonlightVersion);

// ---- B4b (ARBS-W b2g) F1/F2 (D-B4b-1): defaults source ----
// Startup log line: "ARBS-W: 설정 파일 없음 · 마우스 흉내=끔(기본값) · 차단 검사=끔(기본값) · 무음=모드1(기본값)"
// ("설정 파일 있음" / "설정 저장소=레지스트리(포터블 아님)", "켬", "(파일)"). No paths.
std::string prefsSourceLine(const PrefsSource& p);
// --arbs-dump-prefs file body (UTF-8, '\n' lines):
//   edition=<e>\n ini=없음|있음|레지스트리\n gamepadmouse=false 출처=기본값|파일\n detectnetblocking=... \n arbssilence=N 출처=...
std::string prefsDumpText(const PrefsSource& p, const char* edition);

}  // namespace arbsw
