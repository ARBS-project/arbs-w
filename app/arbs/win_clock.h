// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W IClock for Windows (B2 D7, design 4-5 "시계"): QueryPerformanceCounter time base and an absolute-deadline
// sleep built on a per-thread waitable timer. Preferred timer = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION; if the OS
// refuses it, a normal waitable timer; if that fails too, Sleep(). The sleep never returns before the deadline.
#pragma once

#include <atomic>
#include <cstdint>

#include "arbs/platform.h"

namespace arbsw {

class WinClock final : public arbs::IClock {
public:
    enum class TimerMode { Unknown = 0, HighResolution = 1, Normal = 2, SleepOnly = 3 };

    WinClock();
    int64_t nowNs() override;
    void sleepUntilNs(int64_t deadlineNs) override;

    // Mode used by the most recent thread that slept (for the log line). Unknown until the first sleep.
    static TimerMode lastTimerMode();
    static const char* timerModeName(TimerMode m);

private:
    int64_t freq_ = 0;
};

}  // namespace arbsw
