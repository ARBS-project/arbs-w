// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W WinClock. Sources: MS docs CreateWaitableTimerExW / CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+),
// ASB Apex6HapticBridge.cpp:47-81, Vibepollo misc.cpp:3096-3135 (same pattern). B2_DESIGN.md 4-5.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "win_clock.h"

#include "arbs_logic.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace arbsw {

namespace {

std::atomic<int> g_lastMode{0};

// One timer per thread, closed when the thread ends.
struct ThreadTimer {
    HANDLE h = nullptr;
    WinClock::TimerMode mode = WinClock::TimerMode::Unknown;
    ThreadTimer() {
        h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (h != nullptr) {
            mode = WinClock::TimerMode::HighResolution;
        } else {
            h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            mode = h != nullptr ? WinClock::TimerMode::Normal : WinClock::TimerMode::SleepOnly;
        }
        g_lastMode = static_cast<int>(mode);
    }
    ~ThreadTimer() {
        if (h != nullptr) CloseHandle(h);
    }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
};

ThreadTimer& threadTimer() {
    thread_local ThreadTimer t;
    return t;
}

}  // namespace

WinClock::WinClock() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);  // never fails on XP and later
    freq_ = f.QuadPart;
}

int64_t WinClock::nowNs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return ticksToNs(c.QuadPart, freq_);
}

void WinClock::sleepUntilNs(int64_t deadlineNs) {
    ThreadTimer& t = threadTimer();
    for (;;) {
        const int64_t rem = deadlineNs - nowNs();
        if (rem <= 0) return;
        bool waited = false;
        if (t.h != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = waitableDueTime100ns(rem);
            if (SetWaitableTimer(t.h, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(t.h, INFINITE);
                waited = true;
            }
        }
        if (!waited) {
            // Sleep fallback: round up to whole milliseconds (late wake-up allowed, early not).
            const int64_t ms = (rem + 999999) / 1000000;
            Sleep(static_cast<DWORD>(ms > 0 ? ms : 1));
        }
        // loop re-checks: a timer may fire a little early relative to QPC; never return before the deadline.
    }
}

WinClock::TimerMode WinClock::lastTimerMode() { return static_cast<TimerMode>(g_lastMode.load()); }

const char* WinClock::timerModeName(TimerMode m) {
    switch (m) {
    case TimerMode::HighResolution:
        return "고해상도 대기 타이머";
    case TimerMode::Normal:
        return "일반 대기 타이머(고해상도 불가)";
    case TimerMode::SleepOnly:
        return "Sleep 대체(타이머 생성 실패)";
    default:
        return "미사용";
    }
}

}  // namespace arbsw
