// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W Windows layer + bridge logic unit tests (B2 D7/D8). No device access: HID enumeration/opening
// (enumerateApexIf2, WinHidPort::open) is never called here. The port class itself is driven through a local
// overlapped named pipe that stands in for the IF2 handle. Usage: arbs_w_tests.exe <scratch-dir> [filter]
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "arbs/java_compat.h"
#include "arbs_log.h"
#include "arbs_logic.h"
#include "arbs_policy.h"
#include "arbs_settings.h"
#include "mini_test.h"
#include "win_clock.h"
#include "win_hid_port.h"

using namespace arbsw;

namespace {

struct CaptureLog : arbs::ILog {
    std::mutex mu;
    std::vector<std::string> lines;
    void info(const std::string& s) override {
        std::lock_guard<std::mutex> g(mu);
        lines.push_back(s);
    }
    bool perSecondAllowed() override { return true; }
    bool has(const std::string& part) {
        std::lock_guard<std::mutex> g(mu);
        for (const std::string& l : lines)
            if (l.find(part) != std::string::npos) return true;
        return false;
    }
};

HidCandidate cand(const wchar_t* path, uint16_t vid, uint16_t pid, uint16_t page, uint16_t outLen = 33) {
    HidCandidate c;
    c.path = path;
    c.vid = vid;
    c.pid = pid;
    c.usagePage = page;
    c.outLen = outLen;
    c.inLen = 33;
    return c;
}

std::string& scratchDir() {
    static std::string d = ".";
    return d;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ matching
TEST(w_match, path_prefilter) {
    CHECK(pathMayBeApex(L"\\\\?\\hid#vid_37d7&pid_2502&mi_02#8&1a2b3c4d&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"));
    CHECK(pathMayBeApex(L"\\\\?\\HID#VID_37D7&PID_2502&MI_00#x"));
    CHECK(!pathMayBeApex(L"\\\\?\\hid#vid_37d7&pid_2501&mi_02#x"));
    CHECK(!pathMayBeApex(L"\\\\?\\hid#vid_045e&pid_028e#x"));
    CHECK(!pathMayBeApex(L""));
}

TEST(w_match, if2_rule_and_pick) {
    CHECK(isApexIf2(cand(L"a", 0x37D7, 0x2502, 0xFFA0)));
    CHECK(!isApexIf2(cand(L"a", 0x37D7, 0x2502, 0x0001)));  // IF0 (gamepad)
    CHECK(!isApexIf2(cand(L"a", 0x37D7, 0x2501, 0xFFA0)));  // APEX5 dongle
    CHECK(!isApexIf2(cand(L"a", 0x045E, 0x2502, 0xFFA0)));
    std::vector<HidCandidate> one = {cand(L"if0", 0x37D7, 0x2502, 0x0001), cand(L"if2", 0x37D7, 0x2502, 0xFFA0),
                                     cand(L"kbd", 0x046D, 0xC31C, 0x0001)};
    If2Pick p = pickApexIf2(one);
    CHECK_EQ(p.count, 1);
    CHECK_EQ(p.index, 1);
    std::vector<HidCandidate> two = one;
    two.push_back(cand(L"if2b", 0x37D7, 0x2502, 0xFFA0));
    p = pickApexIf2(two);
    CHECK_EQ(p.count, 2);
    CHECK_EQ(p.index, -1);  // two dongles: refuse
    p = pickApexIf2({});
    CHECK_EQ(p.count, 0);
    CHECK_EQ(p.index, -1);
}

TEST(w_match, identify_apex_table) {
    // Exact: SDL reports the real VID/PID and exactly one IF2
    CHECK(identifyApex(0x37D7, 0x2502, 1, 0) == ApexMatch::Exact);
    CHECK(identifyApex(0x37D7, 0x2502, 1, 3) == ApexMatch::Exact);
    CHECK(identifyApex(0x37D7, 0x2502, 0, 0) == ApexMatch::None);  // IF2 missing
    CHECK(identifyApex(0x37D7, 0x2502, 2, 0) == ApexMatch::None);  // two dongles
    // Presumed: XInput fallback 045E/028E, one IF2, one fallback pad
    CHECK(identifyApex(0x045E, 0x028E, 1, 1) == ApexMatch::Presumed);
    CHECK(identifyApex(0x045E, 0x028E, 1, 2) == ApexMatch::None);  // a real Xbox 360 pad as well
    CHECK(identifyApex(0x045E, 0x028E, 0, 1) == ApexMatch::None);
    CHECK(identifyApex(0x045E, 0x028E, 2, 1) == ApexMatch::None);
    // Other pads never match
    CHECK(identifyApex(0x054C, 0x0CE6, 1, 1) == ApexMatch::None);  // DualSense
    CHECK(identifyApex(0x045E, 0x0B12, 1, 1) == ApexMatch::None);  // Xbox Series pad
    CHECK(needsIf2Lookup(0x37D7, 0x2502));
    CHECK(needsIf2Lookup(0x045E, 0x028E));
    CHECK(!needsIf2Lookup(0x054C, 0x0CE6));
    CHECK(std::string(apexMatchName(ApexMatch::Presumed)).find("추정") != std::string::npos);
}

// ------------------------------------------------------------------------------------------------ frames / caps
TEST(w_frame, output_report_33) {
    uint8_t frame[32];
    for (int i = 0; i < 32; i++) frame[i] = static_cast<uint8_t>(0x57 + i);
    uint8_t dst[64];
    std::memset(dst, 0xEE, sizeof(dst));
    CHECK_EQ(buildOutputReport(frame, 33, dst, sizeof(dst)), static_cast<size_t>(33));
    CHECK_EQ(static_cast<int>(dst[0]), 0);
    CHECK(std::memcmp(dst + 1, frame, 32) == 0);
    CHECK_EQ(static_cast<int>(dst[33]), 0xEE);  // untouched past outLen
    // larger report: zero padded (ASB WHT:263-272)
    std::memset(dst, 0xEE, sizeof(dst));
    CHECK_EQ(buildOutputReport(frame, 64, dst, sizeof(dst)), static_cast<size_t>(64));
    CHECK(std::memcmp(dst + 1, frame, 32) == 0);
    for (int i = 33; i < 64; i++) CHECK_EQ(static_cast<int>(dst[i]), 0);
    // smaller report / small buffer / null: refused
    CHECK_EQ(buildOutputReport(frame, 32, dst, sizeof(dst)), static_cast<size_t>(0));
    CHECK_EQ(buildOutputReport(frame, 33, dst, 32), static_cast<size_t>(0));
    CHECK_EQ(buildOutputReport(nullptr, 33, dst, sizeof(dst)), static_cast<size_t>(0));
    CHECK(!outputLenUsable(0));
    CHECK(outputLenUsable(33));
}

TEST(w_frame, caps_and_type) {
    CHECK_EQ(apexCaps(0), 0x8003u);  // same core bits as ARBS-A (OBSERVED EA-A1-U-03)
    CHECK_EQ(apexCaps(0x04), 0x8003u);            // trigger rumble removed
    CHECK_EQ(apexCaps(0x40 | 0x02), 0x8043u);     // battery kept
    CHECK_EQ(apexCaps(0x10 | 0x20), 0x8033u);     // accel/gyro kept
    CHECK_EQ(static_cast<int>(kTypePs), 2);
}

TEST(w_frame, trigger_axis_255) {
    CHECK_EQ(triggerAxisTo255(0), 0);
    CHECK_EQ(triggerAxisTo255(-100), 0);
    CHECK_EQ(triggerAxisTo255(32767), 255);
    CHECK_EQ(triggerAxisTo255(40000), 255);
    for (int v = 0; v <= 32767; v += 97) {
        const int expect = static_cast<int>(arbs::jc::javaRound(v * 255.0 / 32767.0));
        CHECK_EQ(triggerAxisTo255(v), expect);
    }
    CHECK_EQ(triggerAxisTo255(16384), 128);  // 127.5039 -> 128 (upstream truncation gives 127)
}

// ------------------------------------------------------------------------------------------------ hints / log filter
TEST(w_hint, ignore_devices_append) {
    CHECK_EQ(appendIgnoreDevice("", kIgnoreEntry), std::string("0x37d7/0x2502"));
    CHECK_EQ(appendIgnoreDevice("  ", kIgnoreEntry), std::string("0x37d7/0x2502"));
    CHECK_EQ(appendIgnoreDevice("0x1234/0x5678", kIgnoreEntry), std::string("0x1234/0x5678,0x37d7/0x2502"));
    CHECK_EQ(appendIgnoreDevice("0x1234/0x5678,", kIgnoreEntry), std::string("0x1234/0x5678,0x37d7/0x2502"));
    CHECK_EQ(appendIgnoreDevice("0x1234/0x5678, 0x37D7/0x2502", kIgnoreEntry), std::string("0x1234/0x5678, 0x37D7/0x2502"));
    CHECK_EQ(appendIgnoreDevice("0x37d7/0x2502", kIgnoreEntry), std::string("0x37d7/0x2502"));
}

TEST(w_hint, if2_path_warning) {
    CHECK(pathLooksLikeIf2("\\\\?\\HID#VID_37D7&PID_2502&MI_02#8&abc&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}"));
    CHECK(!pathLooksLikeIf2("\\\\?\\HID#VID_37D7&PID_2502&MI_00&Col01#x"));
    CHECK(!pathLooksLikeIf2("XInput#0"));
    CHECK(!pathLooksLikeIf2(""));
}

TEST(w_log, state_line_filter) {
    CHECK(isStateLine("APEX 준비됨(GetInfo 통과). 세션 진행 + 포그라운드일 때 활성화됩니다"));
    CHECK(isStateLine("APEX 햅틱 불가(UNSUPPORTED): 10회 무응답"));
    CHECK(isStateLine("APEX 정지 시퀀스 시작: 사용자 정지"));
    CHECK(isStateLine("ARBS 세션 요약: 총 시간 00:01:00 (세션 종료)"));
    CHECK(isStateLine("ARBS-W: APEX 도착 보고 번호 0"));
    // 1 s / 1 min lines stay in the ARBS log only
    CHECK(!isStateLine("APEX 송신: 125프레임/초(구동 120, 중립 5, rumble 0) 큐=3"));
    CHECK(!isStateLine("APEX 레벨: 리미터동작 0.0% 상한도달 0.0% 게이트닫힘 0.0%"));
    CHECK(!isStateLine("APEX 트리거: 수신 L=0 R=0"));
    CHECK(!isStateLine("ARBS 1분 요약: 경과 00:01:00"));
}

// ------------------------------------------------------------------------------------------------ Win32 arithmetic
TEST(w_win, io_error_classes) {
    CHECK(classifyIoError(1167) == arbs::IoResult::Disconnected);
    CHECK(classifyIoError(433) == arbs::IoResult::Disconnected);
    CHECK(classifyIoError(2) == arbs::IoResult::Disconnected);
    CHECK(classifyIoError(6) == arbs::IoResult::Disconnected);
    CHECK(classifyIoError(995) == arbs::IoResult::Disconnected);
    CHECK(classifyIoError(109) == arbs::IoResult::Disconnected);
    CHECK(classifyIoError(31) == arbs::IoResult::Error);  // ERROR_GEN_FAILURE
    CHECK(classifyIoError(5) == arbs::IoResult::Error);   // ACCESS_DENIED
}

TEST(w_win, due_time_and_ticks) {
    CHECK_EQ(waitableDueTime100ns(0), -1LL);
    CHECK_EQ(waitableDueTime100ns(-5), -1LL);
    CHECK_EQ(waitableDueTime100ns(1), -1LL);
    CHECK_EQ(waitableDueTime100ns(100), -1LL);
    CHECK_EQ(waitableDueTime100ns(101), -2LL);
    CHECK_EQ(waitableDueTime100ns(8000000), -80000LL);
    CHECK_EQ(ticksToNs(10000000, 10000000), 1000000000LL);
    CHECK_EQ(ticksToNs(1, 10000000), 100LL);
    CHECK_EQ(ticksToNs(3, 3), 1000000000LL);
    // 30 days at 10 MHz: no overflow
    const int64_t t = 30LL * 24 * 3600 * 10000000LL + 12345;
    CHECK_EQ(ticksToNs(t, 10000000), 30LL * 24 * 3600 * 1000000000LL + 1234500);
    CHECK_EQ(ticksToNs(5, 0), 0LL);
}

// ------------------------------------------------------------------------------------------------ ring / gate
TEST(w_ring, order_overflow_truncate) {
    ReportRing r;
    uint8_t b[100];
    for (int i = 0; i < 70; i++) {
        std::memset(b, i, sizeof(b));
        r.push(b, 33);
    }
    CHECK_EQ(r.size(), static_cast<size_t>(64));
    CHECK_EQ(r.dropped(), 6ull);
    uint8_t out[64];
    size_t n = 0;
    CHECK(r.pop(out, sizeof(out), &n));
    CHECK_EQ(n, static_cast<size_t>(33));
    CHECK_EQ(static_cast<int>(out[0]), 6);  // oldest 6 dropped
    r.clear();
    CHECK_EQ(r.size(), static_cast<size_t>(0));
    CHECK(!r.pop(out, sizeof(out), &n));
    r.push(b, 100);  // truncated to 64
    CHECK(r.pop(out, sizeof(out), &n));
    CHECK_EQ(n, static_cast<size_t>(64));
    r.push(b, 40);
    CHECK(r.pop(out, 10, &n));  // caller buffer smaller
    CHECK_EQ(n, static_cast<size_t>(10));
}

TEST(w_gate, arrival_only_when_live_once) {
    ArrivalGate g;
    CHECK(g.onApexAdded() == ArrivalGate::Action::Deferred);  // before connection start
    CHECK(g.pending());
    CHECK(g.setLive());                                        // flush exactly once
    CHECK(!g.pending());
    CHECK(g.onApexAdded() == ArrivalGate::Action::AlreadySent);
    CHECK(!g.setLive());
    g.onApexRemoved();                                         // re-plug
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);
    CHECK(g.onApexAdded() == ArrivalGate::Action::AlreadySent);
    g.reset();                                                 // session end
    CHECK(!g.live());
    CHECK(!g.setLive());                                       // nothing pending
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);
    g.reset();
    CHECK(g.onApexAdded() == ArrivalGate::Action::Deferred);
    g.onApexRemoved();                                         // removed before live: nothing to flush
    CHECK(!g.setLive());
}

// ------------------------------------------------------------------------------------------------ settings
TEST(w_settings, asb_defaults_and_normalize) {
    const ArbsSettings d;
    CHECK_EQ(d.amp, 127);
    CHECK_NEAR(d.gain, 1.0, 0);
    CHECK_EQ(d.gatePct, 0);
    CHECK_EQ(d.silence, 1);
    CHECK_EQ(d.curve, 0);
    CHECK_EQ(d.taps, 321);
    CHECK_EQ(d.jitterFixedMs, 0);
    CHECK(d.trigger);
    CHECK_EQ(d.triggerStrength, 100);
    CHECK(!d.verbose);
    int changed = -1;
    ArbsSettings n = settings::normalize(d, &changed);
    CHECK_EQ(changed, 0);
    ArbsSettings bad;
    bad.amp = 128;
    bad.gain = 5.0;
    bad.gatePct = 3;
    bad.silence = 0;
    bad.curve = 2;
    bad.taps = 320;
    bad.jitterFixedMs = 20;
    bad.triggerStrength = 50;
    n = settings::normalize(bad, &changed);
    CHECK_EQ(changed, 8);
    CHECK_EQ(n.amp, 127);
    CHECK_NEAR(n.gain, 1.0, 0);
    CHECK_EQ(n.gatePct, 0);
    CHECK_EQ(n.silence, 1);
    CHECK_EQ(n.curve, 0);
    CHECK_EQ(n.taps, 321);
    CHECK_EQ(n.jitterFixedMs, 0);
    CHECK_EQ(n.triggerStrength, 100);
    ArbsSettings ok;
    ok.amp = 40;
    ok.gain = 4.0000001;
    ok.gatePct = 2;
    ok.silence = 3;
    ok.curve = 1;
    ok.taps = 641;
    ok.jitterFixedMs = 80;
    ok.triggerStrength = 0;
    n = settings::normalize(ok);
    CHECK_EQ(n.amp, 40);
    CHECK_NEAR(n.gain, 4.0, 0);
    CHECK_EQ(n.taps, 641);
    CHECK_EQ(n.jitterFixedMs, 80);
    CHECK_EQ(n.triggerStrength, 0);
}

TEST(w_settings, engine_config_mapping) {
    arbs::Engine::Config c = toEngineConfig(ArbsSettings());
    const arbs::Engine::Config def;
    CHECK_EQ(c.cap, def.cap);
    CHECK_NEAR(c.gain, def.gain, 0);
    CHECK_EQ(c.jitterFixedMs, def.jitterFixedMs);
    CHECK_NEAR(c.gatePct, def.gatePct, 0);
    CHECK_EQ(c.silence, def.silence);
    CHECK_EQ(c.asbCurve, def.asbCurve);
    CHECK_EQ(c.taps, def.taps);
    CHECK_EQ(c.triggerHaptics, def.triggerHaptics);
    CHECK_EQ(c.triggerStrength, def.triggerStrength);
    CHECK_EQ(c.verbose, def.verbose);
    ArbsSettings s;
    s.amp = 60;
    s.gain = 2.0;
    s.gatePct = 2;
    s.silence = 4;
    s.curve = 1;
    s.taps = 161;
    s.jitterFixedMs = 32;
    s.trigger = false;
    s.triggerStrength = 60;
    s.verbose = true;
    c = toEngineConfig(s);
    CHECK_EQ(c.cap, 60);
    CHECK_NEAR(c.gain, 2.0, 0);
    CHECK_NEAR(c.gatePct, 2.0, 0);
    CHECK_EQ(c.silence, 4);
    CHECK(!c.asbCurve);
    CHECK_EQ(c.taps, 161);
    CHECK_EQ(c.jitterFixedMs, 32);
    CHECK(!c.triggerHaptics);
    CHECK_EQ(c.triggerStrength, 60);
    CHECK(c.verbose);
    s.amp = 999;  // out of list -> ASB default even if prefs were not normalised
    CHECK_EQ(toEngineConfig(s).cap, 127);
}

TEST(w_policy, provisional_defaults) {
    // B2_DESIGN recommendations (planning owner decision pending): rumble Core only, minimise = stop
    CHECK(mt::truth(policy::kRumbleCoreOnly));
    CHECK(mt::truth(policy::kPauseOnMinimize));
}

// ------------------------------------------------------------------------------------------------ log file
TEST(w_logfile, write_mirror_crash) {
    const std::string dir = scratchDir();
    const std::wstring path = std::wstring(dir.begin(), dir.end()) + L"\\ARBS-test.log";
    int64_t fake = 1000000000LL;
    std::vector<std::string> mirrored;
    {
        ArbsLogFile log(path, [&] { return fake; });
        CHECK(log.ok());
        log.setMirror(isStateLine, [&](const std::string& s) { mirrored.push_back(s); });
        log.info("APEX 준비됨(GetInfo 통과)");
        fake += 1500000000LL;
        log.info("APEX 송신: 125프레임/초");
        CHECK(log.perSecondAllowed());
        log.setVerbose(true);
        CHECK(log.perSecondAllowed());
        log.setCrashMode();
        log.info("ARBS 세션 요약: 크래시 1");
        CHECK(log.bytesWritten() > 0);
    }
    CHECK_EQ(mirrored.size(), static_cast<size_t>(1));  // crash mode does not mirror
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string txt = ss.str();
    CHECK(txt.find("[+  0.000s] APEX 준비됨(GetInfo 통과)\n") != std::string::npos);
    CHECK(txt.find("[+  1.500s] APEX 송신: 125프레임/초\n") != std::string::npos);
    CHECK(txt.find("ARBS 세션 요약: 크래시 1") != std::string::npos);
    in.close();
    DeleteFileW(path.c_str());
}

// ------------------------------------------------------------------------------------------------ clock
TEST(w_clock, monotonic_and_never_early) {
    WinClock clk;
    int64_t prev = clk.nowNs();
    for (int i = 0; i < 10000; i++) {
        const int64_t t = clk.nowNs();
        CHECK(t >= prev);
        prev = t;
    }
    const int64_t waits[] = {200000, 500000, 1000000, 2000000, 4000000, 8000000};
    int64_t maxLate = 0, sumLate = 0, n = 0;
    for (int64_t w : waits) {
        for (int k = 0; k < 10; k++) {
            const int64_t deadline = clk.nowNs() + w;
            clk.sleepUntilNs(deadline);
            const int64_t late = clk.nowNs() - deadline;
            CHECK(late >= 0);  // never wakes early
            maxLate = std::max(maxLate, late);
            sumLate += late;
            n++;
        }
    }
    clk.sleepUntilNs(clk.nowNs() - 5);  // past deadline returns immediately
    mt::report().push_back(arbs::jc::fmt("w_clock: mode=%s, sleep lateness avg %.3f ms max %.3f ms over %lld waits (informational)",
                                         WinClock::timerModeName(WinClock::lastTimerMode()), sumLate / 1e6 / static_cast<double>(n),
                                         maxLate / 1e6, static_cast<long long>(n)));
    CHECK(WinClock::lastTimerMode() != WinClock::TimerMode::Unknown);
}

// ------------------------------------------------------------------------------------------------ port over a pipe
namespace {

struct PipePair {
    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
};

PipePair makePipe() {
    PipePair p;
    wchar_t name[128];
    swprintf_s(name, L"\\\\.\\pipe\\arbs-w-test-%lu-%llu", GetCurrentProcessId(),
               static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    p.server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0,
                                nullptr);
    if (p.server == INVALID_HANDLE_VALUE) return p;
    p.client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (p.client != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(p.client, &mode, nullptr, nullptr);
    }
    ConnectNamedPipe(p.server, nullptr);  // already connected -> ERROR_PIPE_CONNECTED
    return p;
}

void serverWrite(HANDLE s, const uint8_t* d, DWORD n) {
    DWORD w = 0;
    WriteFile(s, d, n, &w, nullptr);
}

bool waitFor(const std::function<bool()>& f, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return f();
}

}  // namespace

TEST(w_port, write_read_ring_disconnect_over_pipe) {
    PipePair p = makePipe();
    CHECK(p.server != INVALID_HANDLE_VALUE);
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    {
        WinHidPort port(p.client, 33, 33, log, true);
        CHECK(port.exclusive());
        CHECK(log.has("IF2 열림(배타, 출력 33바이트"));

        // write: report ID 0x00 + 32-byte frame on the wire
        arbs::IoResult r;
        uint8_t frame[32];
        for (int i = 0; i < 32; i++) frame[i] = static_cast<uint8_t>(i == 0 ? 0x57 : i);
        r = port.writeFrame(frame, 100);
        CHECK(r == arbs::IoResult::Ok);
        uint8_t got[64];
        DWORD n = 0;
        CHECK(ReadFile(p.server, got, sizeof(got), &n, nullptr) != 0);
        CHECK_EQ(static_cast<int>(n), 33);
        CHECK_EQ(static_cast<int>(got[0]), 0x00);
        CHECK(std::memcmp(got + 1, frame, 32) == 0);

        // read: nothing yet -> Timeout
        uint8_t buf[64];
        size_t len = 99;
        r = port.readFrame(buf, sizeof(buf), &len, 20);
        CHECK(r == arbs::IoResult::Timeout);
        CHECK_EQ(len, static_cast<size_t>(0));

        // read: a 33-byte report (leading 0x00 kept, ApexProtocol.parse accepts it)
        uint8_t rep[33] = {0x00, 0x5A, 0xA5, 0x01};
        serverWrite(p.server, rep, 33);
        r = port.readFrame(buf, sizeof(buf), &len, 500);
        CHECK(r == arbs::IoResult::Ok);
        CHECK_EQ(len, static_cast<size_t>(33));
        CHECK(std::memcmp(buf, rep, 33) == 0);

        // ring: 70 reports without consuming -> 64 kept, 6 dropped, oldest first
        for (int i = 0; i < 70; i++) {
            rep[1] = static_cast<uint8_t>(i);
            serverWrite(p.server, rep, 33);
        }
        CHECK(waitFor([&] { return port.stats().reads >= 71; }, 2000));
        CHECK_EQ(port.stats().ringDropped, 6ull);
        r = port.readFrame(buf, sizeof(buf), &len, 100);
        CHECK(r == arbs::IoResult::Ok);
        CHECK_EQ(static_cast<int>(buf[1]), 6);
        port.clearInput();
        r = port.readFrame(buf, sizeof(buf), &len, 10);
        CHECK(r == arbs::IoResult::Timeout);
        CHECK_EQ(port.stats().writes, 1ull);

        // disconnect: the other end goes away -> reader marks Disconnected, I/O reports it
        CloseHandle(p.server);
        p.server = INVALID_HANDLE_VALUE;
        CHECK(waitFor([&] { return port.disconnected(); }, 2000));
        r = port.readFrame(buf, sizeof(buf), &len, 100);
        CHECK(r == arbs::IoResult::Disconnected);
        r = port.writeFrame(frame, 100);
        CHECK(r == arbs::IoResult::Disconnected);
        CHECK(log.has("IF2 분리 감지"));
    }  // destructor: stop reader, close handle
    CHECK(log.has("IF2 닫힘"));
    if (p.server != INVALID_HANDLE_VALUE) CloseHandle(p.server);
}

TEST(w_port, destructor_stops_idle_reader) {
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    const auto t0 = std::chrono::steady_clock::now();
    {
        WinHidPort port(p.client, 64, 0, log, false);  // inLen 0 -> 64-byte read buffer; out 64 -> zero padded
        uint8_t frame[32] = {0x53, 0x01, 0x12, 0x00};
        CHECK(port.writeFrame(frame, 100) == arbs::IoResult::Ok);
        uint8_t got[128];
        DWORD n = 0;
        CHECK(ReadFile(p.server, got, sizeof(got), &n, nullptr) != 0);
        CHECK_EQ(static_cast<int>(n), 64);
        CHECK(std::memcmp(got + 1, frame, 32) == 0);
        for (int i = 33; i < 64; i++) CHECK_EQ(static_cast<int>(got[i]), 0);
        CHECK(log.has("IF2 열림(공유"));
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 1000);  // pending ReadFile cancelled, thread joined promptly
    CHECK(log.has("IF2 닫힘"));
    CloseHandle(p.server);
}

int main(int argc, char** argv) {
    if (argc > 1) scratchDir() = argv[1];
    const std::string filter = argc > 2 ? argv[2] : "";
    int ok = 0, failed = 0;
    for (const mt::Case& c : mt::registry()) {
        const std::string full = c.group + "." + c.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        try {
            c.fn();
            ok++;
            std::printf("[ OK ] %s\n", full.c_str());
        } catch (const std::exception& e) {
            failed++;
            std::printf("[FAIL] %s : %s\n", full.c_str(), e.what());
        }
        std::fflush(stdout);
    }
    std::printf("\n---- report ----\n");
    for (const std::string& l : mt::report()) std::printf("%s\n", l.c_str());
    std::printf("\n---- summary ----\ncases ok=%d failed=%d checks=%lld\n", ok, failed, mt::checks());
    return failed == 0 ? 0 : 1;
}
