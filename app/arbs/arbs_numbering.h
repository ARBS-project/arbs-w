// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W B2b: APEX-first numbering and host re-report planning (docs/analysis/B2b_APEX_ID.md section 5 2-1/2-2).
// Pure C++ (no Qt / SDL / Win32 / Core), so gamepad.cpp (via input.h) can hold a HostLedger by value and every rule is
// unit-tested (app/arbs/tests). Implementation in arbs_logic.cpp.
//
// Host facts this is built on (Vibepollo 2.18.9, CLAIMED from source, B2b analysis 3-2):
//  - the first arrival for a controller number decides its type/caps (PCM 0x8000 included); a second arrival for an
//    allocated number is ignored ("ControllerNumber already allocated");
//  - an MC packet whose mask lacks a number frees that number.
//  - moonlight-common-c sends each controller number on its own channel: no ordering between numbers.
//
// Numbering decision (planning owner to confirm, analysis 3-3 / K9): CC_TASK_B2b 2-2 says "other pads get later
// numbers", ARBS-A hides the built-in pad completely. This follows the task text: multi-controller mode = APEX on 0,
// other pads 1, 2, ... (not hidden). Single mode ("Force gamepad #1 always connected", every pad on 0) = the arrival
// report of a non-APEX pad is skipped while an APEX is bound or visible, so the APEX PS/PCM arrival is the first one
// for number 0 (risk K1). Input merging on 0 stays upstream.
#pragma once

#include <cstdint>
#include <vector>

namespace arbsw {

// What the host was told about a controller number (from our own arrivals/removals).
enum class HostRole : uint8_t { None = 0, Normal = 1, Apex = 2 };

struct HostStep {
    enum class Kind : uint8_t { Remove, Arrival };
    Kind kind = Kind::Arrival;
    int index = 0;      // controller number
    uint16_t mask = 0;  // active gamepad mask carried by the packet
    bool apex = false;  // Arrival only: PS type + APEX caps (true) or the pad's own SDL type/caps (false)
};

// Per controller number record (main thread only). Remove -> None. Arrival on a free number -> its role; an arrival on
// an allocated number is ignored by the host, so the record stays.
class HostLedger {
public:
    static constexpr int kMax = 16;  // MAX_GAMEPADS
    void reset();
    HostRole role(int index) const;
    int creator(int index) const;  // joystick id whose arrival created [index] (valid when role != None)
    void sent(const HostStep& s, int joystickId);
    void removed(int index);  // upstream DEVICEREMOVED in multi-controller mode (MC packet without the bit)

private:
    HostRole role_[kMax] = {};
    int creator_[kMax] = {};
};

// Slot (= controller number in multi-controller mode) for a new pad.
struct SlotPlan {
    int slot = -1;    // -1 = no free slot
    int moveTo = -1;  // >= 0: move the pad in slot 0 to this slot first (APEX arrived late, multi + APEX first)
};
// [occupied] has [n] entries. Upstream rule = first free slot from 0. Multi + apexFirst:
//  - pad is 37d7/2502: slot 0; if 0 is held by a non-37d7/2502 pad, that pad moves to the first free slot >= 1;
//  - other pad while a 37d7/2502 device is in the SDL list: first free slot from 1 (0 kept for the APEX), 0 if none.
// Single mode or apexFirst off: upstream rule.
SlotPlan planSlot(const bool* occupied, int n, bool multi, bool apexFirst, bool slot0IsExact, bool padIsExact,
                  bool exactInSdlList);

// Single mode + apexFirst: skip the arrival of a pad that is not the APEX while an APEX is bound or a 37d7/2502 device
// is in the SDL list (the APEX arrival must create number 0).
bool skipArrivalSingle(bool multi, bool apexFirst, bool padIsExact, bool padIsApex, bool apexBound, bool exactInSdlList);

// Arrival of [joystickId] on [index] (asApex = bound APEX).
//  multi : number already allocated -> Remove(index, mask without the bit) first, then Arrival(index, mask).
//  single: APEX arrival while 0 was created by a normal pad -> Remove(0, mask 0) first when apexFirst is on or that
//          normal arrival came from this same pad (delayed presumed match, analysis 2-1 step 4 / K8); then Arrival(0).
//          Normal arrival -> Arrival(0) (the host ignores it if 0 is allocated, upstream behaviour).
std::vector<HostStep> planArrival(const HostLedger& l, bool multi, bool apexFirst, int index, bool asApex, int joystickId,
                                  uint16_t mask);
// A pad that stays connected loses the APEX binding (re-bind to another pad).
//  multi : number holds an APEX arrival -> Remove(index) + normal Arrival(index).  single: nothing (0 stays PS/PCM).
//  (APEX-only mode does not use this: the pad that lost the binding is hidden -> planHide, analysis B2c 5 2-1.)
std::vector<HostStep> planUnbind(const HostLedger& l, bool multi, int index, uint16_t mask);

// ---- B2c 2-1: APEX-only mode (setting arbsApexOnly, default on; docs/analysis/B2c.md section 5 2-1) ----
// While the APEX is bound, or a 37d7/2502 game controller is in the SDL list and has not been handled yet (it may still
// bind), every other pad is hidden from the host: no arrival, input, motion, touch or battery, no mask bit (ARBS-A
// ControllerHandler.java:537-544, 3169-3173). A 37d7/2502 pad that was handled and did not bind (IF2 0/2, open failure)
// is no longer "pending" -> the others are shown again (normal Moonlight). Independent of Force #1 (single mode).
//  [exactPending] = analysis "exactInSdlList && !exactBindFailed".
bool hideOther(bool apexOnly, bool padIsApex, bool apexBound, bool exactPending);
// APEX-only on = APEX first numbering whatever the apexFirst setting says (planSlot / planArrival argument).
inline bool apexFirstEffective(bool apexFirst, bool apexOnly) { return apexFirst || apexOnly; }
// Start mask (getAttachedGamepadMask, also the launch gcmap): multi + APEX-only + a 37d7/2502 device in the SDL list ->
// only the APEX bit 0x1 (the other pads will be hidden). Anything else -> [upstreamMask] unchanged.
uint16_t apexOnlyStartMask(bool multi, bool apexOnly, bool exactInSdlList, uint16_t upstreamMask);
// Hide a pad the host may already know about.
//  multi : number [index] was created by our arrival -> Remove(index, mask without the bit); otherwise nothing.
//  single: number 0 was created by a normal arrival -> Remove(0, mask 0) (the APEX arrival then creates 0 as PS/PCM);
//          0 already holds the APEX arrival -> nothing.
std::vector<HostStep> planHide(const HostLedger& l, bool multi, int index, uint16_t mask);
// Show a hidden pad again (APEX gone or never bound): its normal arrival, = planArrival(asApex = false).
//  multi: Arrival(index, mask | bit) (Remove first if the number is somehow allocated). single: Arrival(0, mask).
std::vector<HostStep> planUnhide(const HostLedger& l, bool multi, int index, int joystickId, uint16_t mask);
// Provisional decision D1 (planner has not ruled; analysis recommendation): when the APEX leaves and slot 0 is free,
// the hidden pad in the lowest slot moves to slot 0 before it is shown (1P-only games; its removal MC and arrival then
// share channel 0). [occupiedAfter]/[hidden] have [n] entries (state after the APEX slot is cleared).
// Returns that slot, or -1 (slot 0 still occupied / no hidden pad).
int pickShowMover(const bool* occupiedAfter, const bool* hidden, int n);

}  // namespace arbsw
