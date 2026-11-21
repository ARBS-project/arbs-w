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
std::vector<HidCandidate> enumerateApexIf2(std::string* diag);

class WinHidPort final : public arbs::IApexPort {
public:
    struct Stats {
        uint64_t writes = 0, writeTimeouts = 0, writeErrors = 0, reads = 0, readErrors = 0, ringDropped = 0;
        unsigned long lastError = 0;
    };

    // DEVICE SIGNAL: opens [c.path] (exclusive first, then shared). nullptr on failure, reason in [why].
    static std::unique_ptr<WinHidPort> open(const HidCandidate& c, arbs::ILog& log, std::string* why);

    // B2c 2-2: open policy with injectable pieces (unit tests use fakes or a temp file, never a device).
    //  OpenFn(path, exclusive) = one CreateFileW attempt (GENERIC_READ|WRITE, OVERLAPPED; share 0 or READ|WRITE):
    //  returns the handle (nullptr on failure) and the Win32 error. SleepFn(ms) waits between exclusive retries.
    struct OpenAttempt {
        void* handle = nullptr;    // HANDLE, nullptr = failed
        unsigned long error = 0;   // GetLastError() when failed
    };
    using OpenFn = std::function<OpenAttempt(const std::wstring& path, bool exclusive)>;
    using SleepFn = std::function<void(int ms)>;
    // Error 32 (sharing violation) on the exclusive open -> up to kExclusiveRetries more exclusive tries kRetryDelayMs
    // apart (<= 0.5 s on the main thread), then the shared open as before. The log always gets the number of IF2 ports
    // this process still has open (0 = the other handle is not ARBS-W's own).
    static constexpr int kExclusiveRetries = 5;
    static constexpr int kRetryDelayMs = 100;
    static std::unique_ptr<WinHidPort> openWith(const HidCandidate& c, arbs::ILog& log, std::string* why, const OpenFn& openFn,
                                                const SleepFn& sleepFn);
    // The real single attempt used by open() (CreateFileW). On a non-device path (temp file) it is safe for tests.
    static OpenAttempt createFileAttempt(const std::wstring& path, bool exclusive);
    // Number of WinHidPort objects alive in this process (constructor +1, destructor -1 after CloseHandle).
    static int openCount();

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

    std::vector<uint8_t> wbuf_;
    std::unique_ptr<unsigned char[]> wov_;  // OVERLAPPED storage (kept alive while a write may be pending)
    bool writeStuck_ = false;

    std::thread reader_;
    mutable std::mutex ringMu_;
    std::condition_variable ringCv_;
    ReportRing ring_;
    std::atomic<bool> disconnected_{false};
    std::atomic<int> logBudget_{20};

    mutable std::mutex statsMu_;
    Stats stats_;

    void* mmcss_ = nullptr;  // HANDLE from AvSetMmThreadCharacteristicsW
    bool prioritySet_ = false;

    static std::atomic<int> s_open_;  // B2c 2-2: ports alive in this process
};

}  // namespace arbsw
