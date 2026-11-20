// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W pure logic (B2 D7/D8): matching rules, frame conversion, caps, gates and small helpers that the Windows I/O
// layer (win_hid_port.cpp) and the Moonlight glue (arbs_bridge.cpp) use. No Qt, no SDL, no Win32 here, so every rule
// is unit-tested without touching a device (app/arbs/tests). Design: docs/analysis/B2_DESIGN.md sections 4-3..4-6.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "arbs/engine.h"
#include "arbs/platform.h"
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
