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

#include "arbs/engine.h"
#include "arbs/java_compat.h"
#include "fakes.h"
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

// ================================================================================================ B2b (T1..T13)
// docs/analysis/B2b_APEX_ID.md section 5 2-6. The binder / numbering rules are pure; HostSim below replays the exact
// order gamepad.cpp uses at DEVICEADDED / check / DEVICEREMOVED (slot plan -> move -> bind decision -> moved pad
// arrival -> lost-binding re-report -> this pad's arrival) and records the packets sent to the host.
namespace {

constexpr uint16_t kBuiltinVid = 0x045E, kBuiltinPid = 0x028E;  // AOKZOE A1 Pro built-in pad (SDL XInput fallback)
constexpr uint16_t kApexV = 0x37D7, kApexP = 0x2502;
constexpr int kBuiltinJs = 100, kApexJs = 101, kOtherJs = 102;
using BA = ApexBinder::Action;

struct HostSim {
    bool multi;
    bool apexFirst;
    int if2 = 1;
    bool listExact = false;  // a 37D7/2502 device in the SDL joystick list
    int64_t now = 0;
    HostLedger led;
    ApexBinder binder;
    uint16_t mask;
    struct Slot {
        bool used = false;
        int js = 0;
        uint16_t vid = 0, pid = 0;
        bool skipped = false;
    } slots[16];
    std::vector<std::string> packets;
    int64_t pendingCheck = -1;

    HostSim(bool m, bool a, int attached) : multi(m), apexFirst(a) {
        mask = multi ? static_cast<uint16_t>((1u << attached) - 1) : 1;  // getAttachedGamepadMask()
    }
    int slotOf(int js) const {
        for (int i = 0; i < 16; i++)
            if (slots[i].used && slots[i].js == js) return i;
        return -1;
    }
    int indexOfSlot(int s) const { return multi ? s : 0; }
    bool bound(int js) const { return binder.boundJs() == js; }
    void run(const std::vector<HostStep>& steps, int js) {
        for (const HostStep& s : steps) {
            char b[64];
            if (s.kind == HostStep::Kind::Remove) std::snprintf(b, sizeof b, "R%d/m=0x%x", s.index, s.mask);
            else std::snprintf(b, sizeof b, "A%d:%s/m=0x%x", s.index, s.apex ? "apex" : "norm", s.mask);
            packets.push_back(b);
            led.sent(s, js);
        }
    }
    void arrive(int slot, bool asApex) {
        Slot& p = slots[slot];
        if (skipArrivalSingle(multi, apexFirst, isExactVidPid(p.vid, p.pid), asApex, binder.boundJs() != ApexBinder::kNone, listExact)) {
            p.skipped = true;
            packets.push_back("skip" + std::to_string(p.js));
            return;
        }
        p.skipped = false;
        run(planArrival(led, multi, apexFirst, indexOfSlot(slot), asApex, p.js, mask), p.js);
    }
    void applyLost(int js) {
        const int s = slotOf(js);
        if (s >= 0) run(planUnbind(led, multi, indexOfSlot(s), mask), js);
    }
    void applyBound(int js) {
        const int s = slotOf(js);
        if (s >= 0) arrive(s, true);
    }
    void note(const ApexBinder::Decision& d) {
        if (d.checkAtMs >= 0) pendingCheck = d.checkAtMs;
    }
    ApexBinder::Decision add(int js, uint16_t vid, uint16_t pid) {
        bool occ[16];
        for (int i = 0; i < 16; i++) occ[i] = slots[i].used;
        const bool slot0Exact = occ[0] && isExactVidPid(slots[0].vid, slots[0].pid);
        const SlotPlan plan = planSlot(occ, 16, multi, apexFirst, slot0Exact, isExactVidPid(vid, pid), listExact);
        if (plan.slot < 0) return ApexBinder::Decision();
        if (plan.moveTo >= 0) {
            slots[plan.moveTo] = slots[0];
            slots[0] = Slot();
            mask = static_cast<uint16_t>(mask | (1u << plan.moveTo));
            binder.onIndexChanged(slots[plan.moveTo].js, plan.moveTo);
        }
        Slot& p = slots[plan.slot];
        p.used = true;
        p.js = js;
        p.vid = vid;
        p.pid = pid;
        const int idx = indexOfSlot(plan.slot);
        const ApexBinder::Decision d = binder.onPadAdded(js, idx, vid, pid, now, isExactVidPid(vid, pid) ? if2 : -1, listExact);
        note(d);
        if (multi) mask = static_cast<uint16_t>(mask | (1u << idx));
        if (plan.moveTo >= 0) arrive(plan.moveTo, bound(slots[plan.moveTo].js));
        if (d.unbindJs != ApexBinder::kNone) applyLost(d.unbindJs);
        arrive(plan.slot, bound(js));
        return d;
    }
    ApexBinder::Decision check() {
        const ApexBinder::Decision d = binder.onCheck(now, if2, listExact);
        pendingCheck = -1;
        note(d);
        if (d.unbindJs != ApexBinder::kNone) applyLost(d.unbindJs);
        if (d.bindJs != ApexBinder::kNone) applyBound(d.bindJs);
        return d;
    }
    ApexBinder::Decision remove(int js) {
        const int s = slotOf(js);
        const ApexBinder::Decision d = binder.onPadRemoved(js, now, if2);
        note(d);
        if (s >= 0) {
            const int idx = indexOfSlot(s);
            if (multi) mask = static_cast<uint16_t>(mask & ~(1u << idx));
            char b[32];
            std::snprintf(b, sizeof b, "gone%d/m=0x%x", idx, mask);  // upstream final MC packet
            packets.push_back(b);
            if (multi) led.removed(idx);
            slots[s] = Slot();
        }
        if (d.bindJs != ApexBinder::kNone) applyBound(d.bindJs);
        return d;
    }
    bool has(const std::string& p) const { return std::find(packets.begin(), packets.end(), p) != packets.end(); }
    std::string all() const {
        std::string s;
        for (const std::string& p : packets) s += p + " ";
        return s;
    }
};

}  // namespace

// T1: built-in 045e/028e first, then the APEX 37d7/2502 (both in the SDL list from the start), IF2 1
TEST(b2b_bind, t01_builtin_then_apex_exact_wins) {
    {
        ApexBinder b;
        const auto d1 = b.onPadAdded(kBuiltinJs, 0, kBuiltinVid, kBuiltinPid, 0, -1, true);
        CHECK(d1.action == BA::None);
        CHECK_EQ(d1.checkAtMs, -1LL);  // 37d7/2502 is in the SDL list: no presumed check at all
        CHECK(d1.why.find("추정 매칭 안 함") != std::string::npos);
        const auto d2 = b.onPadAdded(kApexJs, 1, kApexV, kApexP, 3, 1, true);
        CHECK(d2.action == BA::BindExact);
        CHECK_EQ(d2.bindJs, kApexJs);
        CHECK(d2.bindKind == ApexMatch::Exact);
        CHECK_EQ(b.boundJs(), kApexJs);
    }
    {  // multi + APEX first: APEX = 0, built-in = 1
        HostSim h(true, true, 2);
        h.listExact = true;
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.add(kApexJs, kApexV, kApexP);
        CHECK_EQ(h.slotOf(kApexJs), 0);
        CHECK_EQ(h.slotOf(kBuiltinJs), 1);
        CHECK_MSG(h.all() == "A1:norm/m=0x3 A0:apex/m=0x3 ", h.all());
        CHECK(h.led.role(0) == HostRole::Apex);
        CHECK(h.led.role(1) == HostRole::Normal);
    }
    {  // single + APEX first: built-in arrival skipped, APEX arrival creates number 0 (K1)
        HostSim h(false, true, 2);
        h.listExact = true;
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.add(kApexJs, kApexV, kApexP);
        CHECK_MSG(h.all() == "skip100 A0:apex/m=0x1 ", h.all());
        CHECK(h.led.role(0) == HostRole::Apex);
        CHECK_EQ(h.led.creator(0), kApexJs);
        CHECK_EQ(apexCaps(0x40) & 0x8000u, 0x8000u);  // the APEX arrival carries PCM caps
    }
    {  // multi + APEX first off: upstream numbering, identification still exact
        HostSim h(true, false, 2);
        h.listExact = true;
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.add(kApexJs, kApexV, kApexP);
        CHECK_MSG(h.all() == "A0:norm/m=0x3 A1:apex/m=0x3 ", h.all());
        CHECK_EQ(h.binder.boundJs(), kApexJs);
    }
}

// T2: reversed order (APEX first, then the built-in pad): same binding, same numbers
TEST(b2b_bind, t02_apex_then_builtin) {
    {
        ApexBinder b;
        CHECK(b.onPadAdded(kApexJs, 0, kApexV, kApexP, 0, 1, true).action == BA::BindExact);
        const auto d = b.onPadAdded(kBuiltinJs, 1, kBuiltinVid, kBuiltinPid, 2, -1, true);
        CHECK(d.action == BA::None);
        CHECK_EQ(d.checkAtMs, -1LL);
        CHECK_EQ(b.boundJs(), kApexJs);
    }
    {
        HostSim h(true, true, 2);
        h.listExact = true;
        h.add(kApexJs, kApexV, kApexP);
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        CHECK_EQ(h.slotOf(kApexJs), 0);
        CHECK_EQ(h.slotOf(kBuiltinJs), 1);
        CHECK_MSG(h.all() == "A0:apex/m=0x3 A1:norm/m=0x3 ", h.all());
    }
    {
        HostSim h(false, true, 2);
        h.listExact = true;
        h.add(kApexJs, kApexV, kApexP);
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        CHECK_MSG(h.all() == "A0:apex/m=0x1 skip100 ", h.all());
    }
}

// T3: only a 045e/028e pad (APEX behind the XInput fallback) + one IF2: presumed only after 1000 ms
TEST(b2b_bind, t03_fallback_presumed_after_1000ms) {
    ApexBinder b;
    const auto d = b.onPadAdded(kBuiltinJs, 0, kBuiltinVid, kBuiltinPid, 5000, -1, false);
    CHECK(d.action == BA::None);
    CHECK_EQ(d.checkAtMs, 6000LL);
    CHECK(b.checkNeedsIf2());
    const auto early = b.onCheck(5999, 1, false);
    CHECK(early.action == BA::None);
    CHECK_EQ(early.checkAtMs, 6000LL);  // rescheduled
    const auto at = b.onCheck(6000, 1, false);
    CHECK(at.action == BA::BindPresumed);
    CHECK_EQ(at.bindJs, kBuiltinJs);
    CHECK(at.bindKind == ApexMatch::Presumed);
    CHECK(b.boundKind() == ApexMatch::Presumed);
    CHECK(b.onCheck(7000, 1, false).action == BA::None);  // no pending check any more
    // a second fallback pad arriving later does not move the scheduled time earlier
    ApexBinder c;
    CHECK_EQ(c.onPadAdded(1, 0, kBuiltinVid, kBuiltinPid, 0, -1, false).checkAtMs, 1000LL);
    CHECK_EQ(c.onPadAdded(2, 1, 0x054C, 0x0CE6, 400, -1, false).checkAtMs, -1LL);  // DualSense: normal pad
    // the APEX shows up in the SDL list (not yet opened) before the check: no presumed match
    CHECK(c.onCheck(1000, 1, true).action == BA::None);
}

// T4: presumed binding, then the real 37d7/2502 pad appears -> re-bind + host re-reports in channel order
TEST(b2b_bind, t04_presumed_then_exact_rebind_and_reports) {
    {  // multi + APEX first
        HostSim h(true, true, 1);
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        CHECK_EQ(h.pendingCheck, 1000LL);
        h.now = 1000;
        const auto c = h.check();
        CHECK(c.action == BA::BindPresumed);
        h.now = 9000;
        h.listExact = true;
        const auto d = h.add(kApexJs, kApexV, kApexP);
        CHECK(d.action == BA::Rebind);
        CHECK_EQ(d.unbindJs, kBuiltinJs);
        CHECK_EQ(d.unbindIndex, 1);  // already moved to slot 1 before the decision
        CHECK_EQ(d.bindJs, kApexJs);
        CHECK_EQ(d.bindIndex, 0);
        CHECK_EQ(h.slotOf(kApexJs), 0);
        CHECK_EQ(h.slotOf(kBuiltinJs), 1);
        // initial normal arrival, presumed re-report on 0, moved pad on 1 (mask keeps bit 0), then 0 removed + APEX
        CHECK_MSG(h.all() == "A0:norm/m=0x1 R0/m=0x0 A0:apex/m=0x1 A1:norm/m=0x3 R0/m=0x2 A0:apex/m=0x3 ", h.all());
        CHECK(h.led.role(0) == HostRole::Apex);
        CHECK(h.led.role(1) == HostRole::Normal);
        CHECK_EQ(h.binder.indexOf(kBuiltinJs), 1);
    }
    {  // single + APEX first: number 0 already PS/PCM -> no removal, the APEX arrival is a no-op for the host
        HostSim h(false, true, 1);
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.now = 1000;
        CHECK(h.check().action == BA::BindPresumed);
        h.listExact = true;
        CHECK(h.add(kApexJs, kApexV, kApexP).action == BA::Rebind);
        CHECK_MSG(h.all() == "A0:norm/m=0x1 R0/m=0x0 A0:apex/m=0x1 A0:apex/m=0x1 ", h.all());
        CHECK(h.led.role(0) == HostRole::Apex);
    }
    {  // multi + APEX first off: no move; the old number gets a normal arrival again, the APEX keeps its own number
        HostSim h(true, false, 1);
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.now = 1000;
        CHECK(h.check().action == BA::BindPresumed);
        h.listExact = true;
        CHECK(h.add(kApexJs, kApexV, kApexP).action == BA::Rebind);
        CHECK_MSG(h.all() == "A0:norm/m=0x1 R0/m=0x0 A0:apex/m=0x1 R0/m=0x2 A0:norm/m=0x3 A1:apex/m=0x3 ", h.all());
    }
}

// T5: exact APEX bound, the built-in pad is removed -> no detach
TEST(b2b_bind, t05_other_pad_removed_keeps_engine) {
    ApexBinder b;
    b.onPadAdded(kBuiltinJs, 1, kBuiltinVid, kBuiltinPid, 0, -1, true);
    CHECK(b.onPadAdded(kApexJs, 0, kApexV, kApexP, 1, 1, true).action == BA::BindExact);
    CHECK(!b.removalNeedsIf2(kBuiltinJs));
    const auto d = b.onPadRemoved(kBuiltinJs, 2000, -1);
    CHECK(d.action == BA::None);
    CHECK_EQ(d.unbindJs, ApexBinder::kNone);
    CHECK(d.why.find("엔진 영향 없음") != std::string::npos);
    CHECK_EQ(b.boundJs(), kApexJs);
    CHECK(b.onPadRemoved(999, 2001, -1).action == BA::None);  // unknown pad
}

// T6: exact APEX removed -> Unbind; added again -> BindExact and the arrival gate sends again
TEST(b2b_bind, t06_apex_removed_then_readded) {
    ApexBinder b;
    b.onPadAdded(kBuiltinJs, 1, kBuiltinVid, kBuiltinPid, 0, -1, true);
    b.onPadAdded(kApexJs, 0, kApexV, kApexP, 1, 1, true);
    ArrivalGate g;
    CHECK(!g.setLive());
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);
    const auto d = b.onPadRemoved(kApexJs, 5000, -1);
    CHECK(d.action == BA::Unbind);
    CHECK_EQ(d.unbindJs, kApexJs);
    CHECK_EQ(d.unbindIndex, 0);
    CHECK_EQ(d.checkAtMs, -1LL);  // the built-in pad left over is never presumed (37d7/2502 seen this session)
    CHECK_EQ(b.boundJs(), ApexBinder::kNone);
    g.onApexRemoved();
    const auto again = b.onPadAdded(kApexJs + 10, 0, kApexV, kApexP, 8000, 1, true);
    CHECK(again.action == BA::BindExact);
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);
    // without any exact pad seen: removing the bound presumed pad re-checks the remaining candidates later
    ApexBinder p;
    p.onPadAdded(1, 0, kBuiltinVid, kBuiltinPid, 0, -1, false);
    CHECK(p.onCheck(1000, 1, false).action == BA::BindPresumed);
    const auto u = p.onPadRemoved(1, 3000, -1);
    CHECK(u.action == BA::Unbind);
    CHECK_EQ(u.checkAtMs, -1LL);  // no candidate left
    // bound pad removed while another 37d7/2502 pad is open and one IF2 -> re-bind at once
    ApexBinder q;
    q.onPadAdded(1, 0, kApexV, kApexP, 0, 1, true);
    q.onPadAdded(2, 1, kApexV, kApexP, 1, 1, true);  // second exact pad: not bound
    CHECK(q.removalNeedsIf2(1));
    const auto r = q.onPadRemoved(1, 10, 1);
    CHECK(r.action == BA::UnbindThenBindExact);
    CHECK_EQ(r.bindJs, 2);
    CHECK_EQ(r.bindIndex, 1);
    CHECK(q.onPadRemoved(2, 11, -1).action == BA::Unbind);
}

// T7: the candidate disappears within 1000 ms -> the check binds nothing
TEST(b2b_bind, t07_candidate_removed_before_check) {
    ApexBinder b;
    CHECK_EQ(b.onPadAdded(kBuiltinJs, 0, kBuiltinVid, kBuiltinPid, 0, -1, false).checkAtMs, 1000LL);
    CHECK(b.onPadRemoved(kBuiltinJs, 500, -1).action == BA::None);
    CHECK(!b.checkNeedsIf2());
    const auto d = b.onCheck(1000, 1, false);
    CHECK(d.action == BA::None);
    CHECK(d.why.find("후보 없음") != std::string::npos);
    // stale generation after a session end
    const int gen = b.generation();
    b.reset();
    CHECK(b.generation() != gen);
    CHECK_EQ(b.padCount(), static_cast<size_t>(0));
    CHECK(b.onCheck(2000, 1, false).action == BA::None);
}

// T8: two 045e/028e pads + one IF2 -> never presumed
TEST(b2b_bind, t08_two_fallback_pads_refused) {
    ApexBinder b;
    b.onPadAdded(1, 0, kBuiltinVid, kBuiltinPid, 0, -1, false);
    b.onPadAdded(2, 1, kBuiltinVid, kBuiltinPid, 10, -1, false);
    CHECK(!b.checkNeedsIf2());
    const auto d = b.onCheck(1000, 1, false);
    CHECK(d.action == BA::None);
    CHECK(d.why.find("패드 2개") != std::string::npos);
    CHECK_EQ(b.boundJs(), ApexBinder::kNone);
}

// T9: IF2 0 or 2 -> nothing is ever bound
TEST(b2b_bind, t09_if2_zero_or_two) {
    for (int if2 : {0, 2}) {
        ApexBinder b;
        const auto e = b.onPadAdded(kApexJs, 0, kApexV, kApexP, 0, if2, true);
        CHECK(e.action == BA::None);
        CHECK(e.why.find("IF2") != std::string::npos);
        ApexBinder f;
        f.onPadAdded(kBuiltinJs, 0, kBuiltinVid, kBuiltinPid, 0, -1, false);
        CHECK(f.onCheck(1000, if2, false).action == BA::None);
        CHECK_EQ(f.boundJs(), ApexBinder::kNone);
    }
    // other VID/PIDs never enter the binder logic as candidates
    ApexBinder o;
    const auto d = o.onPadAdded(kOtherJs, 0, 0x054C, 0x0CE6, 0, -1, false);
    CHECK(d.action == BA::None);
    CHECK_EQ(d.checkAtMs, -1LL);
}

// T10: SDL3 XInput trigger round trip: IF0-style byte b -> SDL axis -> triggerAxisTo255 == b (Android unit)
TEST(b2b_trigger, t10_sdl_axis_round_trip_256) {
    int same = 0;
    for (int b = 0; b <= 255; b++) {
        // SDL_xinputjoystick.c: axis = b*257 - 32768; SDL_gamepad.c trigger = (axis - min) / (max - min) * 32767 (float, truncated)
        const int sdl = static_cast<int>(static_cast<float>(b * 257) / 65535.0f * 32767.0f);
        if (triggerAxisTo255(sdl) == b) same++;
        else mt::report().push_back(arbs::jc::fmt("t10 mismatch b=%d sdl=%d -> %d", b, sdl, triggerAxisTo255(sdl)));
    }
    CHECK_EQ(same, 256);
    mt::report().push_back(arbs::jc::fmt("b2b t10: SDL axis round trip %d/256 equal", same));
}

// T11: Weapon zone 2..3 str 1 on R2 with the position fed from SDL axis values -> one break + trigger frames.
// Without the feed (the field case: bound pad = untouched built-in pad, R2 = 0) -> 0 breaks, 0 trigger frames.
namespace {
struct WeaponRun {
    int64_t breaks = 0;
    int64_t trigFrames = 0;
};
WeaponRun runWeapon(bool feed) {
    fake::FakeClock clock;
    fake::CaptureLog log(&clock);
    arbs::Engine eng(clock, log);
    const int64_t t0 = clock.nowNs();
    auto port = std::make_unique<fake::FakePort>(clock);
    eng.attach(std::move(port), arbs::Engine::StartMode::Inline);
    eng.setControllerNumber(0);
    eng.onSessionStart(arbs::Engine::Config());
    eng.onInputSeen();
    uint8_t weapon[10] = {0x0C, 0x00, 0x00};  // zones 2,3; strength (0 & 7) + 1 = 1
    const uint8_t zero[10] = {0};
    clock.at(t0 + 300 * fake::MS, [&] { eng.onTriggerEffects(0, 0x04, 0x00, 0x25, zero, weapon); });
    if (feed) {
        // R2 pulled 0 -> full over 120 ms, held, released: same path as ArbsBridge::onApexTriggerAxis
        for (int k = 0; k <= 15; k++) {
            const int v = 32767 * k / 15;
            clock.at(t0 + (400 + 8 * k) * fake::MS, [&eng, v] { eng.onTriggerPosition(0, triggerAxisTo255(v)); });
        }
        for (int k = 0; k <= 15; k++) {
            const int v = 32767 - 32767 * k / 15;
            clock.at(t0 + (800 + 8 * k) * fake::MS, [&eng, v] { eng.onTriggerPosition(0, triggerAxisTo255(v)); });
        }
    }
    clock.setLimit(t0 + 1500 * fake::MS, [&] { eng.requestTerminate("test end"); });
    eng.runLoop();
    WeaponRun r;
    const auto st = eng.trig().stats();
    r.breaks = st[8];
    r.trigFrames = st[11] + st[12] + st[13] + st[14];
    return r;
}
}  // namespace

TEST(b2b_trigger, t11_weapon_break_needs_fed_position) {
    const WeaponRun fed = runWeapon(true);
    const WeaponRun none = runWeapon(false);
    CHECK_EQ(fed.breaks, 1LL);
    CHECK(fed.trigFrames > 0);
    CHECK_EQ(none.breaks, 0LL);
    CHECK_EQ(none.trigFrames, 0LL);
    mt::report().push_back(arbs::jc::fmt("b2b t11: fed weapon breaks %lld trigger frames %lld; not fed %lld / %lld",
                                         static_cast<long long>(fed.breaks), static_cast<long long>(fed.trigFrames),
                                         static_cast<long long>(none.breaks), static_cast<long long>(none.trigFrames)));
}

// T12: slot plan table (multi/single x on/off x occupancy)
TEST(b2b_number, t12_slot_plan_table) {
    bool none[16] = {};
    bool s0[16] = {};
    s0[0] = true;
    bool s01[16] = {};
    s01[0] = s01[1] = true;
    bool full[16];
    for (bool& b : full) b = true;
    bool all_but_0[16];
    for (bool& b : all_but_0) b = true;
    all_but_0[0] = false;
    // on, multi
    CHECK_EQ(planSlot(none, 16, true, true, false, true, true).slot, 0);
    CHECK_EQ(planSlot(none, 16, true, true, false, false, true).slot, 1);  // 0 kept for the APEX
    CHECK_EQ(planSlot(none, 16, true, true, false, false, false).slot, 0);  // no APEX anywhere: upstream
    SlotPlan p = planSlot(s0, 16, true, true, false, true, true);
    CHECK_EQ(p.slot, 0);
    CHECK_EQ(p.moveTo, 1);
    p = planSlot(s01, 16, true, true, false, true, true);
    CHECK_EQ(p.slot, 0);
    CHECK_EQ(p.moveTo, 2);
    p = planSlot(s0, 16, true, true, true, true, true);  // slot 0 already holds a 37d7/2502 pad
    CHECK_EQ(p.slot, 1);
    CHECK_EQ(p.moveTo, -1);
    CHECK_EQ(planSlot(full, 16, true, true, false, true, true).slot, -1);
    CHECK_EQ(planSlot(all_but_0, 16, true, true, false, false, true).slot, 0);  // only 0 left
    // off, multi = upstream
    CHECK_EQ(planSlot(none, 16, true, false, false, false, true).slot, 0);
    p = planSlot(s0, 16, true, false, false, true, true);
    CHECK_EQ(p.slot, 1);
    CHECK_EQ(p.moveTo, -1);
    // single (on or off): upstream slots, numbering is always 0
    p = planSlot(s0, 16, false, true, false, true, true);
    CHECK_EQ(p.slot, 1);
    CHECK_EQ(p.moveTo, -1);
    CHECK_EQ(planSlot(none, 16, false, true, false, false, true).slot, 0);
    // single-mode arrival skip
    CHECK(skipArrivalSingle(false, true, false, false, true, false));
    CHECK(skipArrivalSingle(false, true, false, false, false, true));
    CHECK(!skipArrivalSingle(false, true, false, false, false, false));  // no APEX: upstream
    CHECK(!skipArrivalSingle(false, true, true, false, true, true));     // the 37d7/2502 pad itself
    CHECK(!skipArrivalSingle(false, true, false, true, true, true));     // the bound APEX (presumed)
    CHECK(!skipArrivalSingle(false, false, false, false, true, true));   // setting off
    CHECK(!skipArrivalSingle(true, true, false, false, true, true));     // multi mode
    // ledger: an arrival for an allocated number is ignored, removal frees it
    HostLedger l;
    l.sent(HostStep{HostStep::Kind::Arrival, 0, 1, false}, 7);
    l.sent(HostStep{HostStep::Kind::Arrival, 0, 1, true}, 8);
    CHECK(l.role(0) == HostRole::Normal);
    CHECK_EQ(l.creator(0), 7);
    // single mode: APEX arrival on a normal 0 -> removal (mask 0) first when the setting is on or the same pad made 0
    std::vector<HostStep> v = planArrival(l, false, true, 0, true, 8, 1);
    CHECK_EQ(v.size(), static_cast<size_t>(2));
    CHECK(v[0].kind == HostStep::Kind::Remove && v[0].mask == 0);
    CHECK(v[1].apex && v[1].mask == 1);
    CHECK_EQ(planArrival(l, false, false, 0, true, 8, 1).size(), static_cast<size_t>(1));  // off, other pad: no removal
    CHECK_EQ(planArrival(l, false, false, 0, true, 7, 1).size(), static_cast<size_t>(2));  // off, same pad (K8)
    l.removed(0);
    CHECK(l.role(0) == HostRole::None);
    CHECK_EQ(planUnbind(l, true, 0, 3).size(), static_cast<size_t>(0));
    CHECK_EQ(planUnbind(l, false, 0, 1).size(), static_cast<size_t>(0));
}

// T13: Moonlight RTT samples reach the session summary (was "n/a": nothing called onMoonlightRtt on Windows)
TEST(b2b_rtt, t13_session_summary_has_rtt) {
    for (bool feed : {true, false}) {
        fake::FakeClock clock;
        fake::CaptureLog log(&clock);
        arbs::Engine eng(clock, log);
        const int64_t t0 = clock.nowNs();
        eng.attach(std::make_unique<fake::FakePort>(clock), arbs::Engine::StartMode::Inline);
        eng.setControllerNumber(0);
        eng.onSessionStart(arbs::Engine::Config());
        eng.onInputSeen();
        if (feed) {
            for (int k = 1; k <= 3; k++) clock.at(t0 + k * 100 * fake::MS, [&eng] { eng.onMoonlightRtt(20); });
            clock.at(t0 + 400 * fake::MS, [&eng] { eng.onMoonlightRtt(0); });  // ignored (ms > 0 only)
        }
        clock.at(t0 + 1000 * fake::MS, [&eng] { eng.onSessionEndBegin("테스트"); });
        clock.at(t0 + 1100 * fake::MS, [&eng] { eng.onSessionEndFinish("테스트"); });
        clock.setLimit(t0 + 1500 * fake::MS, [&] { eng.requestTerminate("test end"); });
        eng.runLoop();
        const std::string sum = log.find("ARBS 세션 요약");
        CHECK_MSG(sum.find(feed ? "Moonlight RTT 평균 20ms" : "Moonlight RTT 평균 n/a") != std::string::npos, sum);
    }
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
