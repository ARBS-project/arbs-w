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

bool isExactVidPid(uint16_t sdlVid, uint16_t sdlPid) { return sdlVid == kApexVid && sdlPid == kApexDonglePid; }

bool isFallbackVidPid(uint16_t sdlVid, uint16_t sdlPid) { return sdlVid == kXInputFallbackVid && sdlPid == kXInputFallbackPid; }

// ------------------------------------------------------------------------------------------------ ApexBinder (B2b 2-1)
const char* ApexBinder::actionName(Action a) {
    switch (a) {
    case Action::BindExact:
        return "BindExact";
    case Action::BindPresumed:
        return "BindPresumed";
    case Action::Rebind:
        return "Rebind";
    case Action::Unbind:
        return "Unbind";
    case Action::UnbindThenBindExact:
        return "UnbindThenBindExact";
    default:
        return "None";
    }
}

int ApexBinder::indexOf(int js) const {
    for (const Pad& p : pads_)
        if (p.js == js) return p.index;
    return -1;
}

int ApexBinder::openCount(bool exact) const {
    int n = 0;
    for (const Pad& p : pads_) {
        if (exact ? isExactVidPid(p.vid, p.pid) : isFallbackVidPid(p.vid, p.pid)) n++;
    }
    return n;
}

int64_t ApexBinder::scheduleCheck(int64_t atMs) {
    if (checkPending_) {
        // A timer is already running: it fires early and onCheck() reschedules to the later time.
        if (atMs > checkAtMs_) checkAtMs_ = atMs;
        return -1;
    }
    checkPending_ = true;
    checkAtMs_ = atMs;
    return atMs;
}

ApexBinder::Decision ApexBinder::onPadAdded(int js, int index, uint16_t vid, uint16_t pid, int64_t nowMs, int if2Count,
                                            bool sdlListHasExact) {
    Decision d;
    pads_.erase(std::remove_if(pads_.begin(), pads_.end(), [js](const Pad& p) { return p.js == js; }), pads_.end());
    pads_.push_back(Pad{js, index, vid, pid});
    if (std::find(seen_.begin(), seen_.end(), js) == seen_.end()) seen_.push_back(js);
    if (firstAddMs_ < 0) firstAddMs_ = nowMs;
    if (sdlListHasExact) exactSeen_ = true;
    if (isExactVidPid(vid, pid)) {
        exactSeen_ = true;
        if (identifyApex(vid, pid, if2Count, 0) != ApexMatch::Exact) {
            d.why = arbs::jc::fmt("VID/PID 일치지만 IF2 %d개 - 일반 패드(동글 IF2 가 정확히 1개일 때만 APEX)", if2Count);
            return d;
        }
        if (boundJs_ == kNone) {
            d.action = Action::BindExact;
            d.why = "APEX(VID/PID 일치) - 바로 연결";
        } else if (boundKind_ == ApexMatch::Presumed) {
            d.action = Action::Rebind;
            d.unbindJs = boundJs_;
            d.unbindIndex = indexOf(boundJs_);
            d.why = "APEX(VID/PID 일치) - 추정 연결 패드에서 이 패드로 다시 묶기";
        } else {
            d.why = "APEX 가 이미 연결되어 있음 - 이 패드는 일반 패드로 처리";
            return d;
        }
        d.bindJs = js;
        d.bindIndex = index;
        d.bindKind = ApexMatch::Exact;
        boundJs_ = js;
        boundKind_ = ApexMatch::Exact;
        return d;
    }
    if (isFallbackVidPid(vid, pid)) {
        if (exactSeen_) {
            d.why = "이 세션에서 SDL 이 VID/PID 37d7/2502 장치를 보여 줌 - XInput 대체 VID/PID 추정 매칭 안 함(일반 패드)";
        } else if (boundJs_ != kNone) {
            d.why = "APEX 가 이미 연결되어 있음 - 이 패드는 일반 패드로 처리";
        } else {
            d.checkAtMs = scheduleCheck(std::max(firstAddMs_ + kPresumeDelayMs, nowMs));
            d.why = "추정 후보(XInput 대체 VID/PID) - 첫 패드 도착 1000ms 뒤 확인(VID/PID 일치 패드가 없을 때만 연결)";
        }
        return d;
    }
    d.why = "일반 패드";
    return d;
}

ApexBinder::Decision ApexBinder::onPadRemoved(int js, int64_t nowMs, int if2Count) {
    Decision d;
    auto it = std::find_if(pads_.begin(), pads_.end(), [js](const Pad& p) { return p.js == js; });
    if (it == pads_.end()) {
        d.why = "목록에 없는 패드 제거 - 무시";
        return d;
    }
    const int removedIndex = it->index;
    pads_.erase(it);
    if (js != boundJs_) {
        d.why = "APEX 가 아닌 패드 제거 - 엔진 영향 없음";
        return d;
    }
    d.unbindJs = js;
    d.unbindIndex = removedIndex;
    boundJs_ = kNone;
    boundKind_ = ApexMatch::None;
    for (const Pad& p : pads_) {
        if (!isExactVidPid(p.vid, p.pid)) continue;
        if (if2Count != 1) {
            d.why = arbs::jc::fmt("APEX 패드 제거 - 남은 VID/PID 일치 패드는 IF2 %d개라 연결 안 함", if2Count);
            break;
        }
        d.action = Action::UnbindThenBindExact;
        d.bindJs = p.js;
        d.bindIndex = p.index;
        d.bindKind = ApexMatch::Exact;
        boundJs_ = p.js;
        boundKind_ = ApexMatch::Exact;
        d.why = "APEX 패드 제거 - 남은 VID/PID 일치 패드로 다시 연결";
        return d;
    }
    d.action = Action::Unbind;
    if (d.why.empty()) d.why = "APEX 패드 제거 - 엔진 분리";
    if (!exactSeen_ && openCount(false) > 0) {
        d.checkAtMs = scheduleCheck(std::max(firstAddMs_ + kPresumeDelayMs, nowMs + kPresumeDelayMs));
        d.why += " (추정 후보는 1000ms 뒤 다시 확인)";
    }
    return d;
}

ApexBinder::Decision ApexBinder::onCheck(int64_t nowMs, int if2Count, bool sdlListHasExact) {
    Decision d;
    if (!checkPending_) {
        d.why = "확인 예약 없음 - 무시";
        return d;
    }
    if (nowMs < checkAtMs_) {
        d.checkAtMs = checkAtMs_;  // still pending
        d.why = "확인 시각 전 - 다시 예약";
        return d;
    }
    checkPending_ = false;
    if (sdlListHasExact) exactSeen_ = true;
    if (boundJs_ != kNone) {
        d.why = "APEX 가 이미 연결되어 있음 - 추정 확인 생략";
        return d;
    }
    if (exactSeen_) {
        d.why = "SDL 장치 목록에 VID/PID 37d7/2502 가 있음(또는 이 세션에서 보였음) - 추정 매칭 안 함";
        return d;
    }
    const int nFallback = openCount(false);
    const Pad* cand = nullptr;
    for (const Pad& p : pads_)
        if (isFallbackVidPid(p.vid, p.pid)) cand = &p;
    if (cand == nullptr) {
        d.why = "추정 후보 없음(제거됨)";
        return d;
    }
    if (identifyApex(cand->vid, cand->pid, if2Count, nFallback) != ApexMatch::Presumed) {
        d.why = arbs::jc::fmt("추정 조건 불충족(IF2 %d개, XInput 대체 VID/PID 패드 %d개) - 일반 패드", if2Count, nFallback);
        return d;
    }
    d.action = Action::BindPresumed;
    d.bindJs = cand->js;
    d.bindIndex = cand->index;
    d.bindKind = ApexMatch::Presumed;
    boundJs_ = cand->js;
    boundKind_ = ApexMatch::Presumed;
    d.why = "APEX(추정 매칭: XInput 대체 VID/PID, 동글 1개·패드 1개, 1000ms 동안 VID/PID 일치 패드 없음)";
    return d;
}

void ApexBinder::onBindFailed(int js) {
    if (boundJs_ != js) return;
    boundJs_ = kNone;
    boundKind_ = ApexMatch::None;
}

void ApexBinder::onIndexChanged(int js, int index) {
    for (Pad& p : pads_)
        if (p.js == js) p.index = index;
}

void ApexBinder::reset() {
    pads_.clear();
    seen_.clear();
    boundJs_ = kNone;
    boundKind_ = ApexMatch::None;
    firstAddMs_ = -1;
    checkAtMs_ = -1;
    checkPending_ = false;
    exactSeen_ = false;
    gen_++;
}

bool ApexBinder::seen(int js) const { return std::find(seen_.begin(), seen_.end(), js) != seen_.end(); }

bool ApexBinder::markSeen(int js, uint16_t vid, uint16_t pid) {  // B5 D-B5-4
    if (js < 0 || !isExactVidPid(vid, pid)) return false;
    if (std::find(seen_.begin(), seen_.end(), js) == seen_.end()) seen_.push_back(js);
    return true;
}

bool ApexBinder::removalNeedsIf2(int js) const {
    if (js != boundJs_) return false;
    for (const Pad& p : pads_)
        if (p.js != js && isExactVidPid(p.vid, p.pid)) return true;
    return false;
}

bool ApexBinder::checkNeedsIf2() const { return boundJs_ == kNone && !exactSeen_ && openCount(false) == 1; }

// ------------------------------------------------------------------------------------------------ numbering (B2b 2-2)
void HostLedger::reset() {
    for (int i = 0; i < kMax; i++) {
        role_[i] = HostRole::None;
        creator_[i] = 0;
    }
}

HostRole HostLedger::role(int index) const { return index >= 0 && index < kMax ? role_[index] : HostRole::None; }

int HostLedger::creator(int index) const { return index >= 0 && index < kMax ? creator_[index] : 0; }

void HostLedger::sent(const HostStep& s, int joystickId) {
    if (s.index < 0 || s.index >= kMax) return;
    if (s.kind == HostStep::Kind::Remove) {
        role_[s.index] = HostRole::None;
        return;
    }
    if (role_[s.index] == HostRole::None) {  // an arrival for an allocated number is ignored by the host
        role_[s.index] = s.apex ? HostRole::Apex : HostRole::Normal;
        creator_[s.index] = joystickId;
    }
}

void HostLedger::removed(int index) {
    if (index >= 0 && index < kMax) role_[index] = HostRole::None;
}

SlotPlan planSlot(const bool* occupied, int n, bool multi, bool apexFirst, bool slot0IsExact, bool padIsExact,
                  bool exactInSdlList) {
    auto firstFree = [&](int from) {
        for (int i = from; i < n; i++)
            if (!occupied[i]) return i;
        return -1;
    };
    SlotPlan p;
    if (n <= 0) return p;
    if (!multi || !apexFirst) {
        p.slot = firstFree(0);
        return p;
    }
    if (padIsExact) {
        if (!occupied[0]) {
            p.slot = 0;
        } else if (slot0IsExact) {
            p.slot = firstFree(0);
        } else {
            const int k = firstFree(1);
            if (k >= 0) {
                p.slot = 0;
                p.moveTo = k;
            }
        }
        return p;
    }
    if (exactInSdlList) {
        const int k = firstFree(1);
        p.slot = k >= 0 ? k : firstFree(0);
        return p;
    }
    p.slot = firstFree(0);
    return p;
}

bool skipArrivalSingle(bool multi, bool apexFirst, bool padIsExact, bool padIsApex, bool apexBound, bool exactInSdlList) {
    return !multi && apexFirst && !padIsExact && !padIsApex && (apexBound || exactInSdlList);
}

std::vector<HostStep> planArrival(const HostLedger& l, bool multi, bool apexFirst, int index, bool asApex, int joystickId,
                                  uint16_t mask) {
    std::vector<HostStep> v;
    if (index < 0 || index >= HostLedger::kMax) return v;
    if (multi) {
        const uint16_t bit = static_cast<uint16_t>(1u << index);
        if (l.role(index) != HostRole::None) v.push_back(HostStep{HostStep::Kind::Remove, index, static_cast<uint16_t>(mask & ~bit), false});
        v.push_back(HostStep{HostStep::Kind::Arrival, index, static_cast<uint16_t>(mask | bit), asApex});
        return v;
    }
    if (asApex && l.role(0) == HostRole::Normal && (apexFirst || l.creator(0) == joystickId)) {
        v.push_back(HostStep{HostStep::Kind::Remove, 0, 0, false});
    }
    v.push_back(HostStep{HostStep::Kind::Arrival, 0, mask, asApex});
    return v;
}

std::vector<HostStep> planUnbind(const HostLedger& l, bool multi, int index, uint16_t mask) {
    std::vector<HostStep> v;
    if (!multi || index < 0 || index >= HostLedger::kMax || l.role(index) != HostRole::Apex) return v;
    const uint16_t bit = static_cast<uint16_t>(1u << index);
    v.push_back(HostStep{HostStep::Kind::Remove, index, static_cast<uint16_t>(mask & ~bit), false});
    v.push_back(HostStep{HostStep::Kind::Arrival, index, static_cast<uint16_t>(mask | bit), false});
    return v;
}

// ------------------------------------------------------------------------------------------------ APEX-only (B2c 2-1)
bool hideOther(bool apexOnly, bool padIsApex, bool apexBound, bool exactPending) {
    return apexOnly && !padIsApex && (apexBound || exactPending);
}

uint16_t apexOnlyStartMask(bool multi, bool apexOnly, bool exactInSdlList, uint16_t upstreamMask) {
    if (multi && apexOnly && exactInSdlList && upstreamMask != 0) return 0x1;
    return upstreamMask;
}

std::vector<HostStep> planHide(const HostLedger& l, bool multi, int index, uint16_t mask) {
    std::vector<HostStep> v;
    if (index < 0 || index >= HostLedger::kMax) return v;
    if (multi) {
        const uint16_t bit = static_cast<uint16_t>(1u << index);
        if (l.role(index) != HostRole::None) v.push_back(HostStep{HostStep::Kind::Remove, index, static_cast<uint16_t>(mask & ~bit), false});
        return v;
    }
    if (l.role(0) == HostRole::Normal) v.push_back(HostStep{HostStep::Kind::Remove, 0, 0, false});
    return v;
}

std::vector<HostStep> planUnhide(const HostLedger& l, bool multi, int index, int joystickId, uint16_t mask) {
    return planArrival(l, multi, false, index, false, joystickId, mask);
}

int pickShowMover(const bool* occupiedAfter, const bool* hidden, int n) {
    if (n <= 0 || occupiedAfter == nullptr || hidden == nullptr || occupiedAfter[0]) return -1;
    for (int i = 1; i < n; i++)
        if (occupiedAfter[i] && hidden[i]) return i;
    return -1;
}

// ---- B3 1-1 (B2d): gcpersist, provisional decision D-B3-1 ----
bool persistGamepads(bool multiController, bool apexOnly) { return !multiController || apexOnly; }

std::string persistGamepadsNote(bool multiController, bool apexOnly) {
    const bool p = persistGamepads(multiController, apexOnly);
    const char* why = apexOnly ? (multiController ? "APEX 단독 켬 -> 호스트 DualSense 유지·rebound(잠정 D-B3-1)"
                                                  : "APEX 단독 켬, 단일 모드 = upstream 값")
                               : "APEX 단독 끔 = upstream 값(단일 모드면 1)";
    return arbs::jc::fmt("ARBS-W: 시작 요청 gcpersist=%d(%s)", p ? 1 : 0, why);
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
        "트리거 햅틱 없음",  // B5 D-B5-1
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

bool reopenGateOnIndexChange(ArrivalGate& g, int oldIndex, int newIndex) {
    if (oldIndex < 0 || oldIndex == newIndex) return false;
    g.onApexRemoved();
    return true;
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

// ---- B3 1-2 (B2d): stall lines ----
std::string openStallLine(int64_t stallMs, int retries, const char* result) {
    return arbs::jc::fmt("ARBS-W: IF2 열기 - 메인 스레드 정지 약 %lld ms(배타 재시도 %d회, 결과 %s)", static_cast<long long>(stallMs),
                         retries, result != nullptr ? result : "?");
}

std::string bindStallLine(int64_t totalMs, int64_t cleanupMs, int64_t openMs) {
    return arbs::jc::fmt("ARBS-W: APEX 묶기 메인 스레드 %lld ms(이전 포트 정리 %lld ms + IF2 열기 %lld ms)", static_cast<long long>(totalMs),
                         static_cast<long long>(cleanupMs), static_cast<long long>(openMs));
}

// ---- B3 1-3 (B2d): IF2 detach watch, provisional decision D4 = log only ----
int If2DetachWatch::arm() {
    std::lock_guard<std::mutex> g(mu_);
    gen_++;
    armed_ = true;
    detached_ = noticed_ = warned_ = false;
    detachNs_ = 0;
    firstInputNs_ = -1;
    detachedFlag_ = false;
    return gen_;
}

void If2DetachWatch::disarm() {
    std::lock_guard<std::mutex> g(mu_);
    gen_++;  // a late listener call of the old port is stale
    armed_ = detached_ = false;
    detachedFlag_ = false;
}

bool If2DetachWatch::onDetach(int generation, int64_t nowNs) {
    std::lock_guard<std::mutex> g(mu_);
    if (!armed_ || generation != gen_ || detached_) return false;
    detached_ = true;
    detachNs_ = nowNs;
    detachedFlag_ = true;
    return true;
}

bool If2DetachWatch::onTick(int64_t nowNs) {
    std::lock_guard<std::mutex> g(mu_);
    if (!armed_ || !detached_ || noticed_) return false;
    if ((nowNs - detachNs_) / 1000000LL < kNoticeMs) return false;
    noticed_ = true;
    return true;
}

int64_t If2DetachWatch::onSdlRemoved(int64_t nowNs) {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t ms = armed_ && detached_ ? (nowNs - detachNs_) / 1000000LL : -1;
    gen_++;
    armed_ = detached_ = false;
    detachedFlag_ = false;
    return ms;
}

bool If2DetachWatch::onApexInput(int64_t nowNs) {
    if (!detachedFlag_.load()) return false;  // fast path: every APEX button/axis event comes here
    std::lock_guard<std::mutex> g(mu_);
    if (!armed_ || !detached_ || warned_) return false;
    if (firstInputNs_ < 0) {
        firstInputNs_ = nowNs;
        return false;
    }
    if ((nowNs - firstInputNs_) / 1000000LL <= kInputWarnMs) return false;
    warned_ = true;
    return true;
}

std::string detachRemovedLine(int64_t ms) {
    return arbs::jc::fmt("ARBS-W: IF2 분리 후 SDL 제거까지 %lld ms", static_cast<long long>(ms));
}

std::string detachNoticeLine() {
    return "ARBS-W: IF2 분리 5초 경과 - SDL 제거 아직 없음(숨김 유지 D3; 내장 패드를 쓰려면 동글을 뽑거나 APEX 단독 끄기)";
}

std::string detachInputWarnLine() {
    return "ARBS-W 경고: IF2 분리 상태인데 APEX 입력이 계속 들어옴 - 진동 경로 끊김(동글 다시 꽂기)";
}

// ---- B3 1-4 (B2d): first PCM after the APEX arrival, provisional decision D-B3-4 ----
void PcmWait::arm(int64_t nowNs, int index, Kind kind) {
    std::lock_guard<std::mutex> g(mu_);
    startNs_ = nowNs;
    index_ = index;
    kind_ = kind;
    armed_.store(true);
}

bool PcmWait::onPcm(int64_t nowNs, int index, Result* out) {
    if (!armed_.load()) return false;  // fast path: ~200 packets/s on the receive thread
    std::lock_guard<std::mutex> g(mu_);
    if (!armed_.load() || index != index_) return false;
    armed_.store(false);
    if (out != nullptr) {
        out->ms = (nowNs - startNs_) / 1000000LL;
        out->index = index_;
        out->kind = kind_;
    }
    return true;
}

bool PcmWait::cancel(int64_t nowNs, Result* out) {
    std::lock_guard<std::mutex> g(mu_);
    if (!armed_.load()) return false;
    armed_.store(false);
    if (out != nullptr) {
        out->ms = (nowNs - startNs_) / 1000000LL;
        out->index = index_;
        out->kind = kind_;
    }
    return true;
}

std::string pcmWaitLine(const PcmWait::Result& r) {
    if (r.kind == PcmWait::Kind::AfterReconnect) {
        return arbs::jc::fmt("ARBS-W: 재연결 후 첫 PCM %lld ms(APEX 도착 보고 -> 첫 PCM, 번호 %d)", static_cast<long long>(r.ms), r.index);
    }
    return arbs::jc::fmt("ARBS-W: 접속 후 첫 PCM %lld ms(번호 %d)", static_cast<long long>(r.ms), r.index);
}

std::string pcmWaitNoneLine(const PcmWait::Result& r, const char* reason) {
    return arbs::jc::fmt("ARBS-W: %s 첫 PCM 없음(%lld ms 기다림, 번호 %d, %s)",
                         r.kind == PcmWait::Kind::AfterReconnect ? "재연결 후" : "접속 후", static_cast<long long>(r.ms), r.index,
                         reason != nullptr ? reason : "?");
}

// ---- B3b F2 (b2e): diagnostics only [잠정 D-B3b-3] ----
int apexFrameCommand(const uint8_t frame[32]) {
    if (frame == nullptr || frame[0] != 0x5A || frame[1] != 0xA5) return -1;
    return frame[2];
}

std::string openToDetachLine(int64_t ms, uint64_t getInfoWrites, uint64_t motorRouteWrites) {
    const auto part = [](const char* what, uint64_t n) {
        return n == 0 ? std::string(what) + " 송신 전" : arbs::jc::fmt("%s 송신 후 %llu회", what, static_cast<unsigned long long>(n));
    };
    return arbs::jc::fmt("ARBS-W: IF2 열기 후 분리까지 %lld ms(", static_cast<long long>(ms)) + part("GetInfo", getInfoWrites) + ", " +
           part("0x53", motorRouteWrites) + ")";
}

std::string unbindPortsLine(int openPorts) {
    if (openPorts <= 0) return "ARBS-W: 엔진 분리 뒤 이 프로세스에 열린 IF2 포트 0개";
    return arbs::jc::fmt("ARBS-W 경고: 엔진 분리 뒤 이 프로세스에 열린 IF2 포트 %d개 - 엔진 스레드가 1.5초 안에 끝나지 않아 IF2 핸들이 남음"
                         "(다음 연결 때 정리; 동글 재인식을 막을 수 있음, F2 진단)",
                         openPorts);
}

void HidRecheckWatch::start(int64_t nowNs) {
    std::lock_guard<std::mutex> g(mu_);
    active_ = true;
    firstDone_ = secondDone_ = false;
    startNs_ = nowNs;
}

void HidRecheckWatch::stop() {
    std::lock_guard<std::mutex> g(mu_);
    active_ = false;
}

int64_t HidRecheckWatch::onTick(int64_t nowNs) {
    std::lock_guard<std::mutex> g(mu_);
    if (!active_) return 0;
    const int64_t ms = (nowNs - startNs_) / 1000000LL;
    if (!secondDone_ && ms >= kSecondMs) {
        firstDone_ = secondDone_ = true;
        active_ = false;  // both checkpoints done
        return kSecondMs;
    }
    if (!firstDone_ && ms >= kFirstMs) {
        firstDone_ = true;
        return kFirstMs;
    }
    return 0;
}

bool HidRecheckWatch::active() const {
    std::lock_guard<std::mutex> g(mu_);
    return active_;
}

std::string hidRecheckLine(int64_t checkpointMs, const std::string& hidSummary, int ownPorts, int donglePaths) {
    if (donglePaths == 0) {  // B4 [잠정 D-B4-4]: dongle gone from Windows (pulled) -> info, not a diagnosis
        return arbs::jc::fmt("ARBS-W: IF2 분리 %lld초 뒤 - 동글이 Windows 에 없음(뽑힘이면 정상, 정보) - ",
                             static_cast<long long>(checkpointMs / 1000)) +
               hidSummary + arbs::jc::fmt(", 이 프로세스 IF2 포트 %d개", ownPorts);
    }
    return arbs::jc::fmt("ARBS-W: IF2 분리 %lld초 뒤 SDL 에 37d7/2502 없음 - Windows ", static_cast<long long>(checkpointMs / 1000)) +
           hidSummary + arbs::jc::fmt(", 이 프로세스 IF2 포트 %d개(F2 진단)", ownPorts);
}

// ---- B4 (ARBS-W b2f) [잠정 D-B4-3] ----
#ifndef ARBS_W_EDITION_STR
#define ARBS_W_EDITION_STR "dev"
#endif

const char* editionName() { return ARBS_W_EDITION_STR; }

std::string logHeaderLine(const char* edition, const char* moonlightVersion) {
    return std::string("ARBS-W ") + (edition != nullptr && *edition != '\0' ? edition : "dev") + " 로그 - Moonlight " +
           (moonlightVersion != nullptr ? moonlightVersion : "?") + " + ARBS Core (APEX 햅틱). IP·토큰·PIN 은 기록하지 않음";
}

// ---- B4b (ARBS-W b2g) F1/F2 (D-B4b-1) ----
namespace {
const char* srcWord(bool fromFile) { return fromFile ? "파일" : "기본값"; }
const char* storeWord(const PrefsSource& p) {
    if (!p.iniFormat) return "레지스트리";
    return p.iniExists ? "있음" : "없음";
}
}  // namespace

std::string prefsSourceLine(const PrefsSource& p) {
    const std::string store = p.iniFormat ? std::string("설정 파일 ") + storeWord(p) : std::string("설정 저장소=레지스트리(포터블 아님)");
    return "ARBS-W: " + store + " · 마우스 흉내=" + (p.gamepadMouse ? "켬" : "끔") + "(" + srcWord(p.gamepadMouseFromFile) +
           ") · 차단 검사=" + (p.netBlock ? "켬" : "끔") + "(" + srcWord(p.netBlockFromFile) + ") · 무음=모드" +
           std::to_string(p.silence) + "(" + srcWord(p.silenceFromFile) + ")";
}

std::string prefsDumpText(const PrefsSource& p, const char* edition) {
    std::string s;
    s += std::string("edition=") + (edition != nullptr && *edition != '\0' ? edition : "dev") + "\n";
    s += std::string("ini=") + storeWord(p) + "\n";
    s += std::string("gamepadmouse=") + (p.gamepadMouse ? "true" : "false") + " 출처=" + srcWord(p.gamepadMouseFromFile) + "\n";
    s += std::string("detectnetblocking=") + (p.netBlock ? "true" : "false") + " 출처=" + srcWord(p.netBlockFromFile) + "\n";
    s += "arbssilence=" + std::to_string(p.silence) + " 출처=" + srcWord(p.silenceFromFile) + "\n";
    return s;
}

}  // namespace arbsw
