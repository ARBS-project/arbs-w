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
std::vector<HostStep> planUnbind(const HostLedger& l, bool multi, int index, uint16_t mask);

}  // namespace arbsw
