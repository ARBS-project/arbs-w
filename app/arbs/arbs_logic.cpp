// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W pure logic. See arbs_logic.h.
#include "arbs_logic.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>

#include "arbs/java_compat.h"

namespace arbsw {

namespace {

std::wstring lowerW(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o) c = static_cast<wchar_t>(std::towlower(c));
    return o;
}

std::string lowerA(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

}  // namespace

bool pathMayBeApex(const std::wstring& path) {
    const std::wstring p = lowerW(path);
    return p.find(L"vid_37d7") != std::wstring::npos && p.find(L"pid_2502") != std::wstring::npos;
}

bool isApexIf2(const HidCandidate& c) {
    return c.vid == kApexVid && c.pid == kApexDonglePid && c.usagePage == kIf2UsagePage;
}

If2Pick pickApexIf2(const std::vector<HidCandidate>& list) {
    If2Pick r;
    for (size_t i = 0; i < list.size(); i++) {
        if (!isApexIf2(list[i])) continue;
        r.count++;
        r.index = static_cast<int>(i);
    }
    if (r.count != 1) r.index = -1;
    return r;
}

const char* apexMatchName(ApexMatch m) {
    switch (m) {
    case ApexMatch::Exact:
        return "APEX(VID/PID 일치)";
    case ApexMatch::Presumed:
        return "APEX(추정 매칭: XInput 대체 VID/PID, 동글 1개·패드 1개)";
    default:
        return "일반 패드";
    }
}

ApexMatch identifyApex(uint16_t sdlVid, uint16_t sdlPid, int if2Count, int fallbackPadCount) {
    if (if2Count != 1) return ApexMatch::None;
    if (sdlVid == kApexVid && sdlPid == kApexDonglePid) return ApexMatch::Exact;
    if (sdlVid == kXInputFallbackVid && sdlPid == kXInputFallbackPid && fallbackPadCount == 1) return ApexMatch::Presumed;
    return ApexMatch::None;
}

bool needsIf2Lookup(uint16_t sdlVid, uint16_t sdlPid) {
    return (sdlVid == kApexVid && sdlPid == kApexDonglePid) || (sdlVid == kXInputFallbackVid && sdlPid == kXInputFallbackPid);
}

bool outputLenUsable(uint16_t outLen) { return outLen >= kWireSize; }

size_t buildOutputReport(const uint8_t* frame32, uint16_t outLen, uint8_t* dst, size_t dstCap) {
    if (frame32 == nullptr || dst == nullptr || !outputLenUsable(outLen) || dstCap < outLen) return 0;
    std::memset(dst, 0, outLen);
    dst[0] = 0x00;  // report ID (the IF2 descriptor has no report ID -> 0)
    std::memcpy(dst + 1, frame32, kFrameSize);
    return outLen;
}

uint32_t apexCaps(uint32_t sdlCaps) {
    return (sdlCaps | kCapAnalogTriggers | kCapRumble | kCapHapticsPcm) & ~kCapTriggerRumble;
}

int triggerAxisTo255(int v) {
    if (v <= 0) return 0;
    if (v >= 32767) return 255;
    return static_cast<int>(arbs::jc::javaRound(v * 255.0 / 32767.0));
}

std::string appendIgnoreDevice(const std::string& existing, const std::string& entry) {
    std::string base = trim(existing);
    while (!base.empty() && base.back() == ',') base.pop_back();
    if (base.empty()) return entry;
    // already present? (compare comma-separated items, case-insensitive)
    const std::string want = lowerA(entry);
    size_t start = 0;
    while (start <= base.size()) {
        size_t comma = base.find(',', start);
        if (comma == std::string::npos) comma = base.size();
        if (lowerA(trim(base.substr(start, comma - start))) == want) return base;
        start = comma + 1;
    }
    return base + "," + entry;
}

bool pathLooksLikeIf2(const std::string& sdlPath) {
    const std::string p = lowerA(sdlPath);
    return p.find("vid_37d7") != std::string::npos && p.find("mi_02") != std::string::npos;
}

bool isStateLine(const std::string& line) {
    static const char* const kKeys[] = {
        "준비됨", "햅틱 불가", "UNSUPPORTED", "정지 시퀀스 시작", "정지 완료", "세션 시작", "엔진 스레드 종료", "엔진 종료 요청",
        "다시 연결", "재개 요청", "GetInfo:", "세션 요약", "엔진 예외", "전송 실패", "컨트롤러 번호", "ARBS-W",
    };
    for (const char* k : kKeys) {
        if (line.find(k) != std::string::npos) return true;
    }
    return false;
}

arbs::IoResult classifyIoError(unsigned long e) {
    switch (e) {
    case 1167:  // ERROR_DEVICE_NOT_CONNECTED
    case 433:   // ERROR_NO_SUCH_DEVICE
    case 2:     // ERROR_FILE_NOT_FOUND
    case 6:     // ERROR_INVALID_HANDLE
    case 995:   // ERROR_OPERATION_ABORTED
    case 109:   // ERROR_BROKEN_PIPE
        return arbs::IoResult::Disconnected;
    default:
        return arbs::IoResult::Error;
    }
}

int64_t waitableDueTime100ns(int64_t remainingNs) {
    if (remainingNs <= 0) return -1;
    const int64_t units = (remainingNs + 99) / 100;
    return -std::max<int64_t>(units, 1);
}

int64_t ticksToNs(int64_t ticks, int64_t freq) {
    if (freq <= 0) return 0;
    const int64_t whole = ticks / freq;
    const int64_t rem = ticks % freq;
    return whole * 1000000000LL + (rem * 1000000000LL) / freq;
}

void ReportRing::push(const uint8_t* data, size_t len) {
    if (data == nullptr) return;
    const size_t n = std::min(len, kSlotBytes);
    if (count_ == kSlots) {
        head_ = (head_ + 1) % kSlots;
        count_--;
        dropped_++;
    }
    const size_t tail = (head_ + count_) % kSlots;
    std::memcpy(buf_[tail], data, n);
    len_[tail] = n;
    count_++;
}

bool ReportRing::pop(uint8_t* out, size_t cap, size_t* outLen) {
    if (count_ == 0) return false;
    const size_t n = std::min(len_[head_], cap);
    if (out != nullptr && n > 0) std::memcpy(out, buf_[head_], n);
    if (outLen != nullptr) *outLen = n;
    head_ = (head_ + 1) % kSlots;
    count_--;
    return true;
}

void ReportRing::clear() {
    head_ = 0;
    count_ = 0;
}

ArrivalGate::Action ArrivalGate::onApexAdded() {
    std::lock_guard<std::mutex> g(mu_);
    if (sent_) return Action::AlreadySent;
    if (live_) {
        sent_ = true;
        pending_ = false;
        return Action::SendNow;
    }
    pending_ = true;
    return Action::Deferred;
}

bool ArrivalGate::setLive() {
    std::lock_guard<std::mutex> g(mu_);
    live_ = true;
    if (pending_ && !sent_) {
        pending_ = false;
        sent_ = true;
        return true;
    }
    return false;
}

void ArrivalGate::onApexRemoved() {
    std::lock_guard<std::mutex> g(mu_);
    pending_ = false;
    sent_ = false;
}

void ArrivalGate::reset() {
    std::lock_guard<std::mutex> g(mu_);
    live_ = false;
    pending_ = false;
    sent_ = false;
}

bool ArrivalGate::live() const {
    std::lock_guard<std::mutex> g(mu_);
    return live_;
}

bool ArrivalGate::pending() const {
    std::lock_guard<std::mutex> g(mu_);
    return pending_;
}

arbs::Engine::Config toEngineConfig(const ArbsSettings& in) {
    const ArbsSettings s = settings::normalize(in);
    arbs::Engine::Config c;
    c.cap = s.amp;
    c.gain = static_cast<float>(s.gain);
    c.jitterFixedMs = s.jitterFixedMs;
    c.gatePct = static_cast<float>(s.gatePct);
    c.silence = s.silence;
    c.asbCurve = s.curve == 0;
    c.taps = s.taps;
    c.triggerHaptics = s.trigger;
    c.triggerStrength = s.triggerStrength;
    c.verbose = s.verbose;
    return c;
}

}  // namespace arbsw
