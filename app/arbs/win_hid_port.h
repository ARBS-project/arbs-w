// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W Windows I/O layer for the APEX 6 2.4G dongle IF2 (B2 D7, design docs/analysis/B2_DESIGN.md 4-4, 4-5).
//   find  : SetupDi HID interfaces -> path pre-filter (vid_37d7&pid_2502) -> HidD_GetAttributes + HidP_GetCaps
//           -> VID 37D7, PID 2502, UsagePage 0xFFA0 (ASB WindowsHidTransport.cpp:495-595, Apex5Device.cpp:227-228)
//   open  : GENERIC_READ|WRITE + FILE_FLAG_OVERLAPPED, exclusive (share 0) first, shared R/W if that fails
//   write : report ID 0x00 + 32-byte frame (+ zero padding to OutputReportByteLength), overlapped, timeout,
//           CancelIoEx, 500 ms cancel grace -> "no response" = Disconnected
//   read  : dedicated thread, overlapped ReadFile loop -> ring (64) -> readFrame() waits on a condition variable
//   MMCSS : onEngineThreadStart = "Pro Audio" -> "Games" -> SetThreadPriority(HIGHEST); reverted on end
// Device API calls live only in enumerateApexIf2() and WinHidPort::open() (-> createFileAttempt). Unit tests never call
// them on a device; they drive the same port class through WinHidPort(HANDLE, ...) on a local named pipe, and the B2c
// open policy (openWith) through fakes or createFileAttempt on a temp file (app/arbs/tests).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "arbs/platform.h"
#include "arbs_logic.h"

namespace arbsw {

// DEVICE SIGNAL: enumerates present HID interfaces (opens matching paths with access 0 to read attributes/caps).
// [diag] receives a one-line summary (counts only, no paths). Not used by unit tests.
// B4 [잠정 D-B4-4]: [donglePaths] (optional) receives the number of APEX dongle paths (pre-filter vid_37d7&pid_2502),
// the same count as in [diag]; F2-D3 uses 0 to tell "dongle not in Windows (pulled)" from an SDL-level miss.
std::vector<HidCandidate> enumerateApexIf2(std::string* diag, int* donglePaths = nullptr);

class WinHidPort final : public arbs::IApexPort {
public:
    struct Stats {
        uint64_t writes = 0, writeTimeouts = 0, writeErrors = 0, reads = 0, readErrors = 0, ringDropped = 0;
        unsigned long lastError = 0;
        uint64_t getInfoWrites = 0, motorRouteWrites = 0;  // B3b F2-D1: write attempts of command 0x01 / 0x53
    };

    // DEVICE SIGNAL: opens [c.path] (exclusive first, then shared). nullptr on failure, reason in [why].
    static std::unique_ptr<WinHidPort> open(const HidCandidate& c, arbs::ILog& log, std::string* why);

    // B2c 2-2: open policy with injectable pieces (unit tests use fakes or a temp file, never a device).
    //  OpenFn(path, exclusive) = one CreateFileW attempt (GENERIC_READ|WRITE, OVERLAPPED; share 0 or READ|WRITE):
    //  returns the handle (nullptr on failure) and the Win32 error. SleepFn(ms) is kept as a test seam only: since B3b
    //  the open never waits (kExclusiveRetries = 0, it is never called).
    struct OpenAttempt {
        void* handle = nullptr;    // HANDLE, nullptr = failed
        unsigned long error = 0;   // GetLastError() when failed
    };
    using OpenFn = std::function<OpenAttempt(const std::wstring& path, bool exclusive)>;
    using SleepFn = std::function<void(int ms)>;
    // B3b F3 (ARBS-W b2e, provisional decision D-B3b-2, planner has not ruled; replaces D-B3-2): one exclusive try.
    // Error 32 (sharing violation) -> the shared open at once: no retry, no wait, no later exclusive re-acquire (the
    // 20 x 100 ms retry of b2d blocked the main thread ~2 s and in the field 3/3 still ended in error 32). The warning
    // line is unchanged; the log always gets the number of IF2 ports this process still has open (0 = the other handle
    // is not ARBS-W's own), and whenever error 32 happened the real blocked time ("메인 스레드 정지 약 N ms").
    static constexpr int kExclusiveRetries = 0;
    static std::unique_ptr<WinHidPort> openWith(const HidCandidate& c, arbs::ILog& log, std::string* why, const OpenFn& openFn,
                                                const SleepFn& sleepFn);
    // The real single attempt used by open() (CreateFileW). On a non-device path (temp file) it is safe for tests.
    static OpenAttempt createFileAttempt(const std::wstring& path, bool exclusive);
    // Number of WinHidPort objects alive in this process (constructor +1, destructor -1 after CloseHandle).
    static int openCount();
    // B4 [잠정 D-B4-5] (ARBS-W b2f): error 32 happens on every connection in the field and the shared open works
    // (EA-B3b-U-01), so the long warning is written once per stream session; later error-32 opens in the same session
    // write a short info line. The stall line ("메인 스레드 정지 약 N ms", F3 diagnosis) stays on every error 32.
    // resetSessionNotices() = new session (ArbsBridge::onStreamLive, before any bind of that session; unit tests call it
    // first because the flag is process-wide).
    static void resetSessionNotices();
    static constexpr const char* kSharedAgainLine = "ARBS-W: IF2 공유 모드로 열기(오류 32, 경고는 세션 첫 회만)";

    // Takes ownership of an overlapped handle (used by open() and by the pipe-based unit test).
    WinHidPort(void* overlappedHandle, uint16_t outLen, uint16_t inLen, arbs::ILog& log, bool exclusive);
    ~WinHidPort() override;
    WinHidPort(const WinHidPort&) = delete;
    WinHidPort& operator=(const WinHidPort&) = delete;

    arbs::IoResult writeFrame(const uint8_t frame[32], int timeoutMs) override;
    arbs::IoResult readFrame(uint8_t* buf, size_t cap, size_t* outLen, int timeoutMs) override;
    void clearInput() override;
    void onEngineThreadStart() override;
    void onEngineThreadEnd() override;

    bool exclusive() const { return exclusive_; }
    bool disconnected() const { return disconnected_.load(); }
    // B3 1-3 (ARBS-W B2d): called once, on the thread that first detects the detach (reader thread: ReadFile error 995
    // etc.; engine thread: write path). Set before Engine::attach. If the port is already detached when the listener is
    // set, it is called at once. Must not block (the bridge only records the time).
    // B3b F2-D1 [잠정 D-B3b-3]: right after the "IF2 분리 감지" line the port also logs the time since it was opened
    // (steady clock, constructor) and whether GetInfo / 0x53 had been sent (openToDetachLine).
    void setDisconnectListener(std::function<void()> fn);
    Stats stats() const;

private:
    void readerMain();
    arbs::IoResult failWrite(unsigned long err, const char* where);
    void markDisconnected(unsigned long err, const char* where);

    void* h_ = nullptr;          // HANDLE
    void* writeEvent_ = nullptr; // HANDLE (manual reset)
    void* readEvent_ = nullptr;  // HANDLE (manual reset)
    void* stopEvent_ = nullptr;  // HANDLE (manual reset)
    uint16_t outLen_ = 0, inLen_ = 0;
    arbs::ILog& log_;
    bool exclusive_ = false;
    int64_t openedNs_ = 0;  // B3b F2-D1: steady clock at construction

    std::vector<uint8_t> wbuf_;
    std::unique_ptr<unsigned char[]> wov_;  // OVERLAPPED storage (kept alive while a write may be pending)
    bool writeStuck_ = false;

    std::thread reader_;
    mutable std::mutex ringMu_;
    std::condition_variable ringCv_;
    ReportRing ring_;
    std::atomic<bool> disconnected_{false};
    std::mutex listenerMu_;                    // B3 1-3
    std::function<void()> disconnectListener_;
    bool listenerCalled_ = false;
    std::atomic<int> logBudget_{20};

    mutable std::mutex statsMu_;
    Stats stats_;

    void* mmcss_ = nullptr;  // HANDLE from AvSetMmThreadCharacteristicsW
    bool prioritySet_ = false;

    static std::atomic<int> s_open_;  // B2c 2-2: ports alive in this process
    static std::atomic<bool> s_warned32_;  // B4 D-B4-5: error-32 warning already written in this stream session
};

}  // namespace arbsw
