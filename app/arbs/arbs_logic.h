// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W pure logic (B2 D7/D8): matching rules, frame conversion, caps, gates and small helpers that the Windows I/O
// layer (win_hid_port.cpp) and the Moonlight glue (arbs_bridge.cpp) use. No Qt, no SDL, no Win32 here, so every rule
// is unit-tested without touching a device (app/arbs/tests). Design: docs/analysis/B2_DESIGN.md sections 4-3..4-6.
#pragma once

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

private:
    struct Pad {
        int js;
        int index;
        uint16_t vid, pid;
    };
    int64_t scheduleCheck(int64_t atMs);  // returns the time to report, or -1 if a check is already pending
    int openCount(bool exact) const;

    std::vector<Pad> pads_;
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

// ---- settings -> Core engine config ----
arbs::Engine::Config toEngineConfig(const ArbsSettings& s);

}  // namespace arbsw
