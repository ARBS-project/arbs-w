// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W Windows HID port for the APEX dongle IF2. See win_hid_port.h.
// Protocol facts (33-byte write = report ID 0 + 32, VID/PID/UsagePage, open flags) follow ApexSenseBridge d831575
// (src/transport/WindowsHidTransport.cpp:263-272, 452-484, 495-611; src/flydigi/Apex5Device.cpp:227-228).
// ASB README: MIT; LICENSE file: GPL-3.0-or-later. Only protocol facts are used (same notice as ARBS-A NOTICE).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <hidsdi.h>
#include <setupapi.h>

#include "win_hid_port.h"

#include <chrono>
#include <cstring>

#include "arbs/java_compat.h"
#include "win_clock.h"

namespace arbsw {

namespace {

std::string winErr(DWORD e) { return std::to_string(static_cast<unsigned long>(e)); }

}  // namespace

// ------------------------------------------------------------------------------------------------ enumeration
std::vector<HidCandidate> enumerateApexIf2(std::string* diag) {
    std::vector<HidCandidate> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        if (diag) *diag = "SetupDiGetClassDevs 실패(오류 " + winErr(GetLastError()) + ")";
        return out;
    }
    int interfaces = 0, prefiltered = 0, opened = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA ifd;
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd)) break;
        interfaces++;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<unsigned char> raw(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
        const std::wstring path(detail->DevicePath);
        if (!pathMayBeApex(path)) continue;  // never open unrelated HID devices
        prefiltered++;
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        opened++;
        HidCandidate c;
        c.path = path;
        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        if (HidD_GetAttributes(h, &attr)) {
            c.vid = attr.VendorID;
            c.pid = attr.ProductID;
        }
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps;
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                c.usagePage = caps.UsagePage;
                c.usage = caps.Usage;
                c.inLen = caps.InputReportByteLength;
                c.outLen = caps.OutputReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
        out.push_back(c);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (diag) {
        int if2 = 0;
        for (const HidCandidate& c : out) if2 += isApexIf2(c) ? 1 : 0;
        *diag = "HID 인터페이스 " + std::to_string(interfaces) + "개 중 APEX 동글 경로 " + std::to_string(prefiltered) + "개(열림 " +
                std::to_string(opened) + "), IF2(UsagePage 0xFFA0) " + std::to_string(if2) + "개";
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ open
std::unique_ptr<WinHidPort> WinHidPort::open(const HidCandidate& c, arbs::ILog& log, std::string* why) {
    if (!outputLenUsable(c.outLen)) {
        if (why) *why = "IF2 출력 리포트 길이 " + std::to_string(c.outLen) + " < 33 - 열지 않음";
        return nullptr;
    }
    // Exclusive first (design 4-4): keeps Space Station / XHaptics / SDL from writing to IF2 while ARBS owns it.
    bool exclusive = true;
    HANDLE h = CreateFileW(c.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_SHARING_VIOLATION) {
            log.info("ARBS-W 경고: 다른 프로그램이 IF2 를 쓰는 중(배타 열기 실패, 오류 32) -> 공유 모드로 엽니다. "
                     "Space Station·XHaptics·ApexSenseBridge·Steam 을 종료하세요");
        } else {
            log.info("ARBS-W: IF2 배타 열기 실패(오류 " + winErr(e) + ") -> 공유 모드로 다시 시도");
        }
        exclusive = false;
        h = CreateFileW(c.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (why) *why = "IF2 열기 실패(공유 모드도 실패, 오류 " + winErr(GetLastError()) + ")";
            return nullptr;
        }
    }
    return std::unique_ptr<WinHidPort>(new WinHidPort(h, c.outLen, c.inLen, log, exclusive));
}

// ------------------------------------------------------------------------------------------------ port
WinHidPort::WinHidPort(void* handle, uint16_t outLen, uint16_t inLen, arbs::ILog& log, bool exclusive)
    : h_(handle), outLen_(outLen), inLen_(inLen), log_(log), exclusive_(exclusive) {
    writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wbuf_.assign(outLen_ >= kWireSize ? outLen_ : kWireSize, 0);
    wov_.reset(new unsigned char[sizeof(OVERLAPPED)]);
    std::memset(wov_.get(), 0, sizeof(OVERLAPPED));
    log_.info("ARBS-W: IF2 열림(" + std::string(exclusive_ ? "배타" : "공유") + ", 출력 " + std::to_string(outLen_) + "바이트, 입력 " +
              std::to_string(inLen_) + "바이트)");
    reader_ = std::thread([this] { readerMain(); });
}

WinHidPort::~WinHidPort() {
    if (stopEvent_ != nullptr) SetEvent(static_cast<HANDLE>(stopEvent_));
    if (reader_.joinable()) reader_.join();
    HANDLE h = static_cast<HANDLE>(h_);
    if (writeStuck_ && h != nullptr && h != INVALID_HANDLE_VALUE) {
        CancelIoEx(h, reinterpret_cast<OVERLAPPED*>(wov_.get()));
        WaitForSingleObject(static_cast<HANDLE>(writeEvent_), 500);
    }
    if (h != nullptr && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (writeEvent_) CloseHandle(static_cast<HANDLE>(writeEvent_));
    if (readEvent_) CloseHandle(static_cast<HANDLE>(readEvent_));
    if (stopEvent_) CloseHandle(static_cast<HANDLE>(stopEvent_));
    const Stats s = stats();
    log_.info("ARBS-W: IF2 닫힘 - 쓰기 " + std::to_string(s.writes) + " 시간초과 " + std::to_string(s.writeTimeouts) + " 오류 " +
              std::to_string(s.writeErrors) + ", 읽기 " + std::to_string(s.reads) + " 오류 " + std::to_string(s.readErrors) + " 링폐기 " +
              std::to_string(s.ringDropped));
}

WinHidPort::Stats WinHidPort::stats() const {
    std::lock_guard<std::mutex> g(statsMu_);
    Stats s = stats_;
    {
        std::lock_guard<std::mutex> r(ringMu_);
        s.ringDropped = ring_.dropped();
    }
    return s;
}

void WinHidPort::markDisconnected(unsigned long err, const char* where) {
    if (!disconnected_.exchange(true)) {
        log_.info(std::string("ARBS-W: IF2 분리 감지(") + where + ", 오류 " + std::to_string(err) + ")");
    }
    ringCv_.notify_all();
}

arbs::IoResult WinHidPort::failWrite(unsigned long err, const char* where) {
    {
        std::lock_guard<std::mutex> g(statsMu_);
        stats_.writeErrors++;
        stats_.lastError = err;
    }
    const arbs::IoResult r = classifyIoError(err);
    if (r == arbs::IoResult::Disconnected) markDisconnected(err, where);
    else if (logBudget_.fetch_sub(1) > 0) log_.info(std::string("ARBS-W: IF2 쓰기 오류(") + where + ", 오류 " + std::to_string(err) + ")");
    return r;
}

arbs::IoResult WinHidPort::writeFrame(const uint8_t frame[32], int timeoutMs) {
    if (disconnected_ || writeStuck_) return arbs::IoResult::Disconnected;
    const size_t n = buildOutputReport(frame, outLen_, wbuf_.data(), wbuf_.size());
    if (n == 0) return arbs::IoResult::Error;
    HANDLE h = static_cast<HANDLE>(h_);
    HANDLE ev = static_cast<HANDLE>(writeEvent_);
    OVERLAPPED* ov = reinterpret_cast<OVERLAPPED*>(wov_.get());
    std::memset(ov, 0, sizeof(OVERLAPPED));
    ov->hEvent = ev;
    ResetEvent(ev);
    {
        std::lock_guard<std::mutex> g(statsMu_);
        stats_.writes++;
    }
    if (!WriteFile(h, wbuf_.data(), static_cast<DWORD>(n), nullptr, ov)) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) return failWrite(e, "WriteFile");
        const DWORD w = WaitForSingleObject(ev, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
        if (w == WAIT_TIMEOUT) {
            CancelIoEx(h, ov);
            if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) {
                writeStuck_ = true;  // OVERLAPPED/buffer stay alive (members) until the destructor
                log_.info("ARBS-W: IF2 쓰기 응답 없음(취소 후 500ms 미완료) -> 분리로 처리");
                markDisconnected(WAIT_TIMEOUT, "write cancel");
                return arbs::IoResult::Disconnected;
            }
            DWORD dummy = 0;
            GetOverlappedResult(h, ov, &dummy, FALSE);
            std::lock_guard<std::mutex> g(statsMu_);
            stats_.writeTimeouts++;
            return arbs::IoResult::Timeout;
        }
        if (w != WAIT_OBJECT_0) return failWrite(GetLastError(), "WaitForSingleObject");
    }
    DWORD written = 0;
    if (!GetOverlappedResult(h, ov, &written, FALSE)) return failWrite(GetLastError(), "GetOverlappedResult");
    return written > 0 ? arbs::IoResult::Ok : arbs::IoResult::Error;
}

void WinHidPort::readerMain() {
    HANDLE h = static_cast<HANDLE>(h_);
    HANDLE rev = static_cast<HANDLE>(readEvent_);
    HANDLE stop = static_cast<HANDLE>(stopEvent_);
    std::vector<uint8_t> buf(inLen_ > 0 ? inLen_ : 64);
    HANDLE waits[2] = {rev, stop};
    for (;;) {
        OVERLAPPED ov;
        std::memset(&ov, 0, sizeof(ov));
        ov.hEvent = rev;
        ResetEvent(rev);
        DWORD got = 0;
        DWORD err = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov)) {
            err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                if (classifyIoError(err) == arbs::IoResult::Disconnected) {
                    markDisconnected(err, "ReadFile");
                    return;
                }
                {
                    std::lock_guard<std::mutex> g(statsMu_);
                    stats_.readErrors++;
                    stats_.lastError = err;
                }
                if (WaitForSingleObject(stop, 10) == WAIT_OBJECT_0) return;
                continue;
            }
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &got, TRUE);
                return;
            }
        }
        if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
            err = GetLastError();
            if (classifyIoError(err) == arbs::IoResult::Disconnected) {
                if (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) markDisconnected(err, "ReadFile 완료");
                return;
            }
            {
                std::lock_guard<std::mutex> g(statsMu_);
                stats_.readErrors++;
                stats_.lastError = err;
            }
            if (WaitForSingleObject(stop, 10) == WAIT_OBJECT_0) return;
            continue;
        }
        if (got > 0) {
            {
                std::lock_guard<std::mutex> g(ringMu_);
                ring_.push(buf.data(), got);
            }
            {
                std::lock_guard<std::mutex> g(statsMu_);
                stats_.reads++;
            }
            ringCv_.notify_one();
        }
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
    }
}

arbs::IoResult WinHidPort::readFrame(uint8_t* buf, size_t cap, size_t* outLen, int timeoutMs) {
    if (outLen != nullptr) *outLen = 0;
    std::unique_lock<std::mutex> lk(ringMu_);
    const auto ready = [this] { return ring_.size() > 0 || disconnected_.load(); };
    if (!ringCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0), ready)) return arbs::IoResult::Timeout;
    if (ring_.pop(buf, cap, outLen)) return arbs::IoResult::Ok;
    return arbs::IoResult::Disconnected;
}

void WinHidPort::clearInput() {
    std::lock_guard<std::mutex> g(ringMu_);
    ring_.clear();
}

void WinHidPort::onEngineThreadStart() {
    DWORD task = 0;
    HANDLE t = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    std::string how;
    if (t != nullptr) {
        how = "MMCSS \"Pro Audio\"";
    } else {
        const DWORD e1 = GetLastError();
        task = 0;
        t = AvSetMmThreadCharacteristicsW(L"Games", &task);
        if (t != nullptr) {
            how = "MMCSS \"Games\"(Pro Audio 실패 오류 " + winErr(e1) + ")";
        } else {
            const DWORD e2 = GetLastError();
            prioritySet_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
            how = "SetThreadPriority(HIGHEST) " + std::string(prioritySet_ ? "성공" : "실패") + "(MMCSS 실패 오류 " + winErr(e1) + "/" + winErr(e2) + ")";
        }
    }
    mmcss_ = t;
    log_.info("ARBS-W: 엔진 스레드 우선순위 = " + how + ", 시계 = " + WinClock::timerModeName(WinClock::lastTimerMode()));
}

void WinHidPort::onEngineThreadEnd() {
    if (mmcss_ != nullptr) {
        AvRevertMmThreadCharacteristics(static_cast<HANDLE>(mmcss_));
        mmcss_ = nullptr;
    } else if (prioritySet_) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        prioritySet_ = false;
    }
}

}  // namespace arbsw
