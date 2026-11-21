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
#include "arbs/protocol.h"
#include "fakes.h"
#include "arbs_log.h"
#include "arbs_logic.h"
#include "arbs_policy.h"
#include "arbs_settings.h"
#include "mini_test.h"
#include "win_clock.h"
#include "win_hid_port.h"
// B3b F1: the exact merge rule compiled into moonlight-common-c InputStream.c (relative to this file)
#include "../../../moonlight-common-c/moonlight-common-c/src/ArbsMcBatch.h"

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
        // B2c 2-3 (verify B2b #24): the flag is set before the log line is written -> wait for the line (test only)
        CHECK(waitFor([&] { return log.has("IF2 분리 감지"); }, 2000));
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
    // B2c 2-1 (APEX-only). Off = B2b behaviour (T1..T13 unchanged).
    bool apexOnly = false;
    std::vector<int> listExactIds;  // instance ids of the 37D7/2502 game controllers in the SDL list
    bool openFails = false;         // WinHidPort::open fails in bindTo -> ApexBinder::onBindFailed
    struct Slot {
        bool used = false;
        int js = 0;
        uint16_t vid = 0, pid = 0;
        bool skipped = false;
        bool hidden = false;  // GamepadState::arbsHidden
        int buttons = 0;
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
    bool anyBound() const { return binder.boundJs() != ApexBinder::kNone; }
    // ArbsBridge::exactPending(): a 37D7/2502 game controller in the SDL list that the binder has not seen yet
    bool pending() const {
        for (int id : listExactIds)
            if (!binder.seen(id)) return true;
        return false;
    }
    // input.cpp: APEX-only on -> start mask (getAttachedGamepadMask)
    void only() {
        apexOnly = true;
        mask = apexOnlyStartMask(multi, true, listExact, mask);
    }
    void unplugExact(int id) {
        listExactIds.erase(std::remove(listExactIds.begin(), listExactIds.end(), id), listExactIds.end());
        listExact = !listExactIds.empty();
    }
    // gamepad.cpp arbsMovePad
    void move(int from, int to, bool forApex) {
        slots[to] = slots[from];
        slots[from] = Slot();
        if (!slots[to].hidden) mask = static_cast<uint16_t>(mask | (1u << to));
        binder.onIndexChanged(slots[to].js, to);
        packets.push_back(forApex ? "mv" + std::to_string(from) + ">" + std::to_string(to)
                                  : "mv" + std::to_string(from) + ">" + std::to_string(to) + "(D1)");
    }
    // gamepad.cpp sendGamepadState: nothing for a hidden pad; single mode merges the visible pads only
    void input(int js, int buttons) {
        const int s = slotOf(js);
        if (s < 0) return;
        slots[s].buttons = buttons;
        sendState(s);
    }
    void sendState(int s) {
        if (slots[s].hidden) return;
        int b = slots[s].buttons;
        if (!multi) {
            for (const Slot& o : slots)
                if (o.used && !o.hidden) b |= o.buttons;
        }
        char t[48];
        std::snprintf(t, sizeof t, "in%d:b=0x%x/m=0x%x", indexOfSlot(s), b, mask);
        packets.push_back(t);
    }
    // battery / motion / touch / mouse emulation: one guard in gamepad.cpp (state->arbsHidden -> return)
    bool sendsAux(int js) const {
        const int s = slotOf(js);
        return s >= 0 && !slots[s].hidden;
    }
    // gamepad.cpp arbsHidePad / arbsShowPad / arbsSyncHidden / arbsPrepareShow
    void hide(int s) {
        const int idx = indexOfSlot(s);
        if (multi) mask = static_cast<uint16_t>(mask & ~(1u << idx));
        run(planHide(led, multi, idx, mask), slots[s].js);
        slots[s].hidden = true;
        packets.push_back("hide" + std::to_string(slots[s].js));
    }
    void show(int s) {
        const int idx = indexOfSlot(s);
        slots[s].hidden = false;
        if (multi) mask = static_cast<uint16_t>(mask | (1u << idx));
        packets.push_back("show" + std::to_string(slots[s].js));
        run(planUnhide(led, multi, idx, slots[s].js, mask), slots[s].js);
        sendState(s);
    }
    void sync(int skip) {
        if (!apexOnly) return;
        const bool b = anyBound(), p = pending();
        if (!hideOther(true, false, b, p) && multi) {
            bool occ[16], hid[16];
            for (int i = 0; i < 16; i++) {
                occ[i] = slots[i].used;
                hid[i] = occ[i] && slots[i].hidden && i != skip;
            }
            const int mover = pickShowMover(occ, hid, 16);
            if (mover >= 0) move(mover, 0, false);
        }
        for (int i = 0; i < 16; i++) {
            if (!slots[i].used || i == skip) continue;
            const bool apex = bound(slots[i].js);
            const bool want = hideOther(true, apex, b, p);
            if (want && !slots[i].hidden) hide(i);
            else if (!want && slots[i].hidden && !apex) show(i);
        }
    }
    void prepareShow(int removed) {
        if (!apexOnly || !multi || slots[removed].hidden) return;
        const bool hiding = hideOther(true, false, anyBound(), pending());
        bool occ[16], hid[16];
        for (int i = 0; i < 16; i++) {
            occ[i] = slots[i].used && i != removed;
            hid[i] = occ[i] && slots[i].hidden;
        }
        const int mover = hiding ? -1 : pickShowMover(occ, hid, 16);
        for (int i = 0; i < 16; i++) {
            if (!hid[i] || i == mover) continue;
            if (!hiding || bound(slots[i].js)) mask = static_cast<uint16_t>(mask | (1u << i));
        }
    }
    // gamepad.cpp arbsApplyBind (check / removal paths)
    void applyBind(const ApexBinder::Decision& d) {
        if (d.unbindJs != ApexBinder::kNone) applyLost(d.unbindJs);
        sync(-1);
        if (d.bindJs != ApexBinder::kNone && bound(d.bindJs)) {
            const int s = slotOf(d.bindJs);
            if (s >= 0 && slots[s].hidden) {
                slots[s].hidden = false;
                if (multi) mask = static_cast<uint16_t>(mask | (1u << s));
            }
            applyBound(d.bindJs);
        }
    }
    void bridgeExecute(const ApexBinder::Decision& d) {
        // arbs_bridge.cpp bindTo failure -> onBindFailed (the binder forgets the binding)
        if (openFails && d.bindJs != ApexBinder::kNone) binder.onBindFailed(d.bindJs);
    }
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
        if (p.hidden) return;  // B2c: arbsSendArrival returns false for a hidden pad
        if (!apexOnly &&
            skipArrivalSingle(multi, apexFirst, isExactVidPid(p.vid, p.pid), asApex, binder.boundJs() != ApexBinder::kNone, listExact)) {
            p.skipped = true;
            packets.push_back("skip" + std::to_string(p.js));
            return;
        }
        p.skipped = false;
        run(planArrival(led, multi, apexFirstEffective(apexFirst, apexOnly), indexOfSlot(slot), asApex, p.js, mask), p.js);
    }
    void applyLost(int js) {
        const int s = slotOf(js);
        if (s >= 0 && !apexOnly) run(planUnbind(led, multi, indexOfSlot(s), mask), js);
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
        const SlotPlan plan =
            planSlot(occ, 16, multi, apexFirstEffective(apexFirst, apexOnly), slot0Exact, isExactVidPid(vid, pid), listExact);
        if (plan.slot < 0) return ApexBinder::Decision();
        if (plan.moveTo >= 0) {
            if (apexOnly) {
                move(0, plan.moveTo, true);
            } else {  // B2b path, unchanged
                slots[plan.moveTo] = slots[0];
                slots[0] = Slot();
                mask = static_cast<uint16_t>(mask | (1u << plan.moveTo));
                binder.onIndexChanged(slots[plan.moveTo].js, plan.moveTo);
            }
        }
        Slot& p = slots[plan.slot];
        p.used = true;
        p.js = js;
        p.vid = vid;
        p.pid = pid;
        p.hidden = false;
        const int idx = indexOfSlot(plan.slot);
        const ApexBinder::Decision d = binder.onPadAdded(js, idx, vid, pid, now, isExactVidPid(vid, pid) ? if2 : -1, listExact);
        bridgeExecute(d);
        note(d);
        p.hidden = hideOther(apexOnly, bound(js), anyBound(), pending());
        if (multi) mask = p.hidden ? static_cast<uint16_t>(mask & ~(1u << idx)) : static_cast<uint16_t>(mask | (1u << idx));
        if (apexOnly) {
            if (p.hidden) packets.push_back("hide" + std::to_string(js));
            sync(plan.slot);
        }
        if (plan.moveTo >= 0) arrive(plan.moveTo, bound(slots[plan.moveTo].js));
        if (d.unbindJs != ApexBinder::kNone) applyLost(d.unbindJs);
        arrive(plan.slot, bound(js));
        return d;
    }
    ApexBinder::Decision check() {
        const ApexBinder::Decision d = binder.onCheck(now, if2, listExact);
        bridgeExecute(d);
        pendingCheck = -1;
        note(d);
        applyBind(d);
        return d;
    }
    ApexBinder::Decision remove(int js) {
        const int s = slotOf(js);
        const ApexBinder::Decision d = binder.onPadRemoved(js, now, if2);
        bridgeExecute(d);
        note(d);
        if (s >= 0 && slots[s].hidden) {  // B2c: the host never knew this pad
            packets.push_back("hgone" + std::to_string(js));
            slots[s] = Slot();
            applyBind(d);
            return d;
        }
        if (s >= 0) {
            prepareShow(s);
            const int idx = indexOfSlot(s);
            if (multi) mask = static_cast<uint16_t>(mask & ~(1u << idx));
            char b[32];
            std::snprintf(b, sizeof b, "gone%d/m=0x%x", idx, mask);  // upstream final MC packet
            packets.push_back(b);
            if (multi) led.removed(idx);
            slots[s] = Slot();
        }
        applyBind(d);
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

// ================================================================================================ B2c (H1..H13, P2..P4)
// docs/analysis/B2c.md section 5 2-4. APEX-only mode (setting arbsApexOnly): HostSim replays gamepad.cpp's B2c order
// (slot plan -> move -> bind -> hide/show sync -> moved pad -> this pad; removal: prepareShow -> removal MC -> D1 move ->
// show). "hide<js>"/"show<js>" = the ARBS log line of the transition, "in<n>:b=/m=" = an MC input packet.
namespace {
constexpr int kDsJs = 102;  // a DualSense next to a presumed APEX (H9)
HostSim simOnly(bool multi, bool apexFirst, int attached, bool apexInList, int apexId = kApexJs) {
    HostSim h(multi, apexFirst, attached);
    if (apexInList) {
        h.listExact = true;
        h.listExactIds = {apexId};
    }
    h.only();
    return h;
}
}  // namespace

// pure rules: hideOther / start mask / planHide / planUnhide / pickShowMover / binder seen / setting default
TEST(b2c_hide, rules_table) {
    // hidden only with the setting on, never the APEX itself, only while the APEX is bound or a 37d7/2502 pad is pending
    CHECK(hideOther(true, false, true, false));
    CHECK(hideOther(true, false, false, true));
    CHECK(!hideOther(true, false, false, false));  // no APEX (or bind failed): normal Moonlight
    CHECK(!hideOther(true, true, true, true));     // the APEX
    CHECK(!hideOther(false, false, true, true));   // setting off = B2b
    CHECK(apexFirstEffective(false, true));
    CHECK(apexFirstEffective(true, false));
    CHECK(!apexFirstEffective(false, false));
    CHECK_EQ(static_cast<int>(apexOnlyStartMask(true, true, true, 0x3)), 0x1);
    CHECK_EQ(static_cast<int>(apexOnlyStartMask(true, true, false, 0x3)), 0x3);  // no APEX in the list
    CHECK_EQ(static_cast<int>(apexOnlyStartMask(true, false, true, 0x3)), 0x3);  // setting off
    CHECK_EQ(static_cast<int>(apexOnlyStartMask(false, true, true, 0x1)), 0x1);  // single mode is always 0x1
    CHECK_EQ(static_cast<int>(apexOnlyStartMask(true, true, true, 0x0)), 0x0);   // nothing attached
    HostLedger l;
    CHECK_EQ(planHide(l, true, 1, 0x1).size(), static_cast<size_t>(0));  // host never knew number 1
    l.sent(HostStep{HostStep::Kind::Arrival, 1, 0x3, false}, 100);
    std::vector<HostStep> v = planHide(l, true, 1, 0x1);
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK(v[0].kind == HostStep::Kind::Remove && v[0].index == 1 && v[0].mask == 0x1);
    l.sent(HostStep{HostStep::Kind::Arrival, 0, 0x1, false}, 100);
    v = planHide(l, false, 0, 0x1);  // single: normal 0 -> Remove(0, mask 0)
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK(v[0].kind == HostStep::Kind::Remove && v[0].index == 0 && v[0].mask == 0);
    HostLedger a;
    a.sent(HostStep{HostStep::Kind::Arrival, 0, 0x1, true}, 101);
    CHECK_EQ(planHide(a, false, 0, 0x1).size(), static_cast<size_t>(0));  // single: 0 already PS/PCM
    v = planUnhide(HostLedger(), true, 2, 100, 0x1);
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK(v[0].kind == HostStep::Kind::Arrival && !v[0].apex && v[0].index == 2 && v[0].mask == 0x5);
    v = planUnhide(a, false, 0, 100, 0x1);  // single: Arrival(0) (the host ignores it while 0 is allocated)
    CHECK_EQ(v.size(), static_cast<size_t>(1));
    CHECK(!v[0].apex && v[0].index == 0);
    bool occ[16] = {}, hid[16] = {};
    occ[1] = occ[3] = true;
    hid[1] = hid[3] = true;
    CHECK_EQ(pickShowMover(occ, hid, 16), 1);
    occ[0] = true;
    CHECK_EQ(pickShowMover(occ, hid, 16), -1);  // slot 0 still taken
    occ[0] = false;
    hid[1] = hid[3] = false;
    CHECK_EQ(pickShowMover(occ, hid, 16), -1);  // nothing hidden
    ApexBinder b;
    CHECK(!b.seen(kApexJs));
    b.onPadAdded(kApexJs, 0, kApexV, kApexP, 0, 0, true);  // IF2 0: not bound, but seen (no longer pending)
    CHECK(b.seen(kApexJs));
    CHECK_EQ(b.boundJs(), ApexBinder::kNone);
    b.onPadRemoved(kApexJs, 1, -1);
    CHECK(b.seen(kApexJs));  // ids are not reused within a session
    b.reset();
    CHECK(!b.seen(kApexJs));
    CHECK(mt::truth(ArbsSettings().apexOnly));  // default on
    CHECK(mt::truth(ArbsSettings().apexFirst));
}

// H1: built-in pad then APEX (both in the SDL list at start), multi + APEX-only: the host only ever sees the APEX
TEST(b2c_hide, h01_builtin_then_apex_multi) {
    HostSim h = simOnly(true, true, 2, true);
    CHECK_EQ(static_cast<int>(h.mask), 0x1);  // start mask (gcmap) without the built-in pad
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
    h.add(kApexJs, kApexV, kApexP);
    CHECK_EQ(h.slotOf(kApexJs), 0);
    CHECK_EQ(h.slotOf(kBuiltinJs), 1);
    CHECK_MSG(h.all() == "hide100 A0:apex/m=0x1 ", h.all());
    h.input(kBuiltinJs, 0x1000);  // A on the built-in pad: nothing to the host
    h.input(kApexJs, 0x2000);
    CHECK_MSG(h.all() == "hide100 A0:apex/m=0x1 in0:b=0x2000/m=0x1 ", h.all());
    CHECK(h.led.role(0) == HostRole::Apex);
    CHECK(h.led.role(1) == HostRole::None);
    CHECK(!h.sendsAux(kBuiltinJs));  // battery / motion / touch / mouse emulation
    CHECK(h.sendsAux(kApexJs));
}

// H2: reversed order (APEX first): same host result
TEST(b2c_hide, h02_apex_then_builtin_multi) {
    HostSim h = simOnly(true, true, 2, true);
    h.add(kApexJs, kApexV, kApexP);
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
    CHECK_EQ(h.slotOf(kApexJs), 0);
    CHECK_EQ(h.slotOf(kBuiltinJs), 1);
    CHECK_MSG(h.all() == "A0:apex/m=0x1 hide100 ", h.all());
    CHECK_EQ(static_cast<int>(h.mask), 0x1);
    CHECK(h.led.role(1) == HostRole::None);
}

// H3: APEX connected later (built-in pad already reported as number 0; dongle not plugged at the check)
TEST(b2c_hide, h03_apex_later) {
    for (bool multi : {true, false}) {
        HostSim h = simOnly(multi, true, 1, false);
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.if2 = 0;  // no dongle yet: the 1000 ms presumed check binds nothing
        h.now = 1000;
        CHECK(h.check().action == BA::None);
        h.now = 5000;
        h.if2 = 1;
        h.listExact = true;
        h.listExactIds = {kApexJs};
        CHECK(h.add(kApexJs, kApexV, kApexP).action == BA::BindExact);
        if (multi) {
            // built-in moved to slot 1 (struct only) and hidden; number 0 removed (mask 0) then APEX arrival, both channel 0
            CHECK_MSG(h.all() == "A0:norm/m=0x1 mv0>1 hide100 R0/m=0x0 A0:apex/m=0x1 ", h.all());
            CHECK_EQ(h.slotOf(kBuiltinJs), 1);
            CHECK_EQ(h.binder.indexOf(kBuiltinJs), 1);
        } else {
            CHECK_MSG(h.all() == "A0:norm/m=0x1 R0/m=0x0 hide100 A0:apex/m=0x1 ", h.all());
        }
        CHECK_EQ(static_cast<int>(h.mask), 0x1);
        CHECK(h.led.role(0) == HostRole::Apex);
        CHECK_EQ(h.led.creator(0), kApexJs);
    }
}

// H4 + H5: APEX removed -> built-in pad shown again on number 0 (D1); APEX plugged again -> hidden again, APEX on 0
TEST(b2c_hide, h04_h05_apex_removed_then_reconnected) {
    HostSim h = simOnly(true, true, 2, true);
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
    h.add(kApexJs, kApexV, kApexP);
    h.packets.clear();
    h.input(kBuiltinJs, 0x1000);
    h.unplugExact(kApexJs);
    const auto d = h.remove(kApexJs);
    CHECK(d.action == BA::Unbind);
    // removal MC frees 0 (mask without bit 0), the hidden pad moves to 0 and arrives on the same channel 0
    CHECK_MSG(h.all() == "gone0/m=0x0 mv1>0(D1) show100 A0:norm/m=0x1 in0:b=0x1000/m=0x1 ", h.all());
    CHECK_EQ(h.slotOf(kBuiltinJs), 0);
    CHECK_EQ(h.binder.indexOf(kBuiltinJs), 0);
    CHECK(h.led.role(0) == HostRole::Normal);
    h.packets.clear();
    h.input(kBuiltinJs, 0x0);  // built-in input goes to the host again
    CHECK_MSG(h.all() == "in0:b=0x0/m=0x1 ", h.all());
    // H5: reconnect (new SDL instance id), gate re-opened by the removal
    ArrivalGate g;
    CHECK(!g.setLive());
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);
    g.onApexRemoved();
    h.packets.clear();
    const int again = kApexJs + 10;
    h.listExact = true;
    h.listExactIds = {again};
    CHECK(h.add(again, kApexV, kApexP).action == BA::BindExact);
    CHECK_MSG(h.all() == "mv0>1 hide100 R0/m=0x0 A0:apex/m=0x1 ", h.all());
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);
    CHECK_EQ(static_cast<int>(h.mask), 0x1);
    h.packets.clear();
    h.input(kBuiltinJs, 0x1000);
    CHECK_MSG(h.all() == "", h.all());
}

// H6: single mode ("Force gamepad #1" on) + APEX-only: no built-in arrival, its input not merged; after the APEX leaves
// the built-in pad drives number 0 again
TEST(b2c_hide, h06_single_mode) {
    HostSim h = simOnly(false, true, 2, true);
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
    h.add(kApexJs, kApexV, kApexP);
    CHECK_MSG(h.all() == "hide100 A0:apex/m=0x1 ", h.all());
    h.input(kBuiltinJs, 0x1000);
    h.input(kApexJs, 0x2000);  // not merged with the built-in pad's 0x1000
    CHECK_MSG(h.all() == "hide100 A0:apex/m=0x1 in0:b=0x2000/m=0x1 ", h.all());
    h.packets.clear();
    h.unplugExact(kApexJs);
    h.remove(kApexJs);
    CHECK_MSG(h.all() == "gone0/m=0x1 show100 A0:norm/m=0x1 in0:b=0x1000/m=0x1 ", h.all());
    CHECK(h.led.role(0) == HostRole::Apex);  // the host keeps its PS/PCM number 0 (normal arrival ignored)
}

// H7: APEX-only off = B2b (the same HostSim with the new code paths reproduces T1/T2/T4 strings)
TEST(b2c_hide, h07_off_keeps_b2b) {
    {
        HostSim h(true, true, 2);
        h.listExact = true;
        h.listExactIds = {kApexJs};  // ignored while apexOnly is off
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.add(kApexJs, kApexV, kApexP);
        CHECK_MSG(h.all() == "A1:norm/m=0x3 A0:apex/m=0x3 ", h.all());
        h.input(kBuiltinJs, 0x1000);
        CHECK_MSG(h.all() == "A1:norm/m=0x3 A0:apex/m=0x3 in1:b=0x1000/m=0x3 ", h.all());
    }
    {
        HostSim h(false, true, 2);
        h.listExact = true;
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.add(kApexJs, kApexV, kApexP);
        CHECK_MSG(h.all() == "skip100 A0:apex/m=0x1 ", h.all());
        h.input(kApexJs, 0x2000);  // B2b single mode merges the built-in pad (0 buttons here)
        h.input(kBuiltinJs, 0x1000);
        CHECK_MSG(h.all() == "skip100 A0:apex/m=0x1 in0:b=0x2000/m=0x1 in0:b=0x3000/m=0x1 ", h.all());
    }
    {
        HostSim h(true, false, 1);
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.now = 1000;
        CHECK(h.check().action == BA::BindPresumed);
        h.listExact = true;
        CHECK(h.add(kApexJs, kApexV, kApexP).action == BA::Rebind);
        CHECK_MSG(h.all() == "A0:norm/m=0x1 R0/m=0x0 A0:apex/m=0x1 R0/m=0x2 A0:norm/m=0x3 A1:apex/m=0x3 ", h.all());
    }
    {  // APEX removed with the setting off: the built-in pad keeps number 1, nothing re-reported
        HostSim h(true, true, 2);
        h.listExact = true;
        h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        h.add(kApexJs, kApexV, kApexP);
        h.packets.clear();
        h.remove(kApexJs);
        CHECK_MSG(h.all() == "gone0/m=0x2 ", h.all());
    }
}

// H8: 37d7/2502 in the list but not bindable (IF2 0, IF2 2, or the open fails) -> hidden pads are shown again
TEST(b2c_hide, h08_bind_failure_shows_again) {
    for (int mode = 0; mode < 3; mode++) {
        for (bool multi : {true, false}) {
            HostSim h = simOnly(multi, true, 2, true);
            if (mode == 0) h.if2 = 0;
            if (mode == 1) h.if2 = 2;
            if (mode == 2) h.openFails = true;
            h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
            h.add(kApexJs, kApexV, kApexP);
            CHECK_EQ(h.binder.boundJs(), ApexBinder::kNone);
            if (multi) {
                CHECK_MSG(h.all() == "hide100 show100 A1:norm/m=0x3 in1:b=0x0/m=0x3 A0:norm/m=0x3 ", h.all());
            } else {
                CHECK_MSG(h.all() == "hide100 show100 A0:norm/m=0x1 in0:b=0x0/m=0x1 A0:norm/m=0x1 ", h.all());
            }
        }
    }
}

// H9: presumed APEX (045e/028e, no 37d7/2502 anywhere): other pads visible until the 1000 ms check binds it
TEST(b2c_hide, h09_presumed_bind_hides_others) {
    HostSim h = simOnly(true, true, 2, false);
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);  // the APEX behind the XInput fallback
    h.add(kDsJs, 0x054C, 0x0CE6);
    CHECK_MSG(h.all() == "A0:norm/m=0x3 A1:norm/m=0x3 ", h.all());
    h.now = 1000;
    CHECK(h.check().action == BA::BindPresumed);
    CHECK_MSG(h.all() == "A0:norm/m=0x3 A1:norm/m=0x3 R1/m=0x1 hide102 R0/m=0x0 A0:apex/m=0x1 ", h.all());
    CHECK(h.led.role(1) == HostRole::None);
    CHECK_EQ(static_cast<int>(h.mask), 0x1);
}

// H10: presumed -> exact re-bind with APEX-only: the pad that lost the binding is only removed (no normal re-arrival)
TEST(b2c_hide, h10_rebind_lost_pad_hidden) {
    HostSim h = simOnly(true, true, 1, false);
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
    h.now = 1000;
    CHECK(h.check().action == BA::BindPresumed);
    h.now = 9000;
    h.listExact = true;
    h.listExactIds = {kApexJs};
    const auto d = h.add(kApexJs, kApexV, kApexP);
    CHECK(d.action == BA::Rebind);
    CHECK_MSG(h.all() == "A0:norm/m=0x1 R0/m=0x0 A0:apex/m=0x1 mv0>1 hide100 R0/m=0x0 A0:apex/m=0x1 ", h.all());
    CHECK(h.all().find("A1:norm") == std::string::npos);
    CHECK_EQ(static_cast<int>(h.mask), 0x1);
    CHECK(h.led.role(0) == HostRole::Apex);
    CHECK_EQ(h.led.creator(0), kApexJs);
}

// H11 (verify B2b #6): the bound APEX moves to another number -> the gate is re-opened so that arrival is sent
TEST(b2c_hide, h11_gate_reopen_on_index_change) {
    ArrivalGate g;
    CHECK(!g.setLive());
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);  // APEX (presumed) arrival on 0
    ArrivalGate old;
    CHECK(!old.setLive());
    CHECK(old.onApexAdded() == ArrivalGate::Action::SendNow);
    CHECK(old.onApexAdded() == ArrivalGate::Action::AlreadySent);  // B2b: the moved number's arrival was dropped
    CHECK(reopenGateOnIndexChange(g, 0, 1));
    CHECK(g.onApexAdded() == ArrivalGate::Action::SendNow);  // B2c: sent on the new number
    CHECK(!reopenGateOnIndexChange(g, 1, 1));                // same number: nothing
    CHECK(!reopenGateOnIndexChange(g, -1, 0));               // first bind: nothing
    CHECK(g.onApexAdded() == ArrivalGate::Action::AlreadySent);
    // the B2b scenario that needs it: presumed APEX on 0, a 37d7/2502 pad that cannot bind (IF2 2) takes slot 0
    HostSim h(true, true, 1);
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
    h.now = 1000;
    CHECK(h.check().action == BA::BindPresumed);
    h.listExact = true;
    h.if2 = 2;
    CHECK(h.add(kApexJs, kApexV, kApexP).action == BA::None);
    CHECK_EQ(h.slotOf(kBuiltinJs), 1);
    CHECK(h.all().find("A1:apex/m=0x3") != std::string::npos);  // the arrival the gate must let through
}

// H12: a hidden pad sends no battery / motion / touch / mouse emulation / input, also after a hide on a live pad
TEST(b2c_hide, h12_hidden_pad_sends_nothing) {
    HostSim h = simOnly(true, true, 1, false);
    h.add(kDsJs, 0x054C, 0x0CE6);  // visible (no APEX yet)
    CHECK(h.sendsAux(kDsJs));
    h.listExact = true;
    h.listExactIds = {kApexJs};
    h.add(kApexJs, kApexV, kApexP);  // DualSense was number 0 -> moved + hidden
    CHECK(!h.sendsAux(kDsJs));
    h.packets.clear();
    for (int b : {0x1, 0x2, 0x0}) h.input(kDsJs, b);
    CHECK_MSG(h.all() == "", h.all());
    // removing the hidden pad sends nothing to the host either
    h.remove(kDsJs);
    CHECK_MSG(h.all() == "hgone102 ", h.all());
    CHECK_EQ(static_cast<int>(h.mask), 0x1);
}

// H13: APEX-only is independent of Force #1 (single/multi) and of the APEX first setting
TEST(b2c_hide, h13_independent_of_force1_and_apex_first) {
    for (bool apexFirst : {true, false}) {
        HostSim m = simOnly(true, apexFirst, 2, true);
        m.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        m.add(kApexJs, kApexV, kApexP);
        CHECK_MSG(m.all() == "hide100 A0:apex/m=0x1 ", m.all());  // = H1
        HostSim s = simOnly(false, apexFirst, 2, true);
        s.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
        s.add(kApexJs, kApexV, kApexP);
        CHECK_MSG(s.all() == "hide100 A0:apex/m=0x1 ", s.all());  // = H6
    }
}

// ------------------------------------------------------------------------------------------------ B2c 2-2 IF2 open
namespace {
struct FakeOpen {
    std::vector<unsigned long> exclusiveErrors;  // one entry per exclusive attempt; 0 = success
    unsigned long sharedError = 0;
    HANDLE handle = INVALID_HANDLE_VALUE;
    int exclusiveCalls = 0, sharedCalls = 0, sleptMs = 0;
    WinHidPort::OpenFn fn() {
        return [this](const std::wstring&, bool exclusive) {
            WinHidPort::OpenAttempt a;
            unsigned long e = sharedError;
            if (exclusive) {
                e = exclusiveCalls < static_cast<int>(exclusiveErrors.size()) ? exclusiveErrors[static_cast<size_t>(exclusiveCalls)]
                                                                               : exclusiveErrors.back();
                exclusiveCalls++;
            } else {
                sharedCalls++;
            }
            if (e == 0) a.handle = handle;
            else a.error = e;
            return a;
        };
    }
    WinHidPort::SleepFn sleep() {
        return [this](int ms) { sleptMs += ms; };
    }
};
}  // namespace

// P2 (B3b F3 [잠정 D-B3b-2], was "error 32 twice then exclusive after retries"): error 32 once -> shared at once, even
// though a second exclusive try would have succeeded (no retry, no wait, no re-acquire)
TEST(b2c_if2, p02_busy_once_then_shared_at_once) {
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    FakeOpen f;
    f.exclusiveErrors = {32, 0};
    f.handle = p.client;
    const int base = WinHidPort::openCount();
    std::string why;
    {
        std::unique_ptr<WinHidPort> port = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log, &why, f.fn(), f.sleep());
        CHECK(port != nullptr);
        CHECK(!port->exclusive());
        CHECK_EQ(WinHidPort::openCount(), base + 1);
    }
    CHECK_EQ(WinHidPort::openCount(), base);
    CHECK_EQ(f.exclusiveCalls, 1);
    CHECK_EQ(f.sharedCalls, 1);
    CHECK_EQ(f.sleptMs, 0);
    CHECK(log.has("IF2 열기 - 이 프로세스에 이미 열린 IF2 포트 " + std::to_string(base) + "개"));
    CHECK(!log.has("다시 시도"));          // b2d retry line gone
    CHECK(!log.has("배타 열기 성공(재시도"));
    CHECK(log.has("다른 프로그램이 IF2 를 쓰는 중"));
    CHECK(log.has("IF2 열림(공유"));
    CloseHandle(p.server);
}

// P3: error 32 on the exclusive try -> shared open at once (B3b F3: was after <= 2 s), warning kept + "0 ports of this process"
TEST(b2c_if2, p03_exclusive_always_busy_shared_fallback) {
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    FakeOpen f;
    f.exclusiveErrors = {32};
    f.handle = p.client;
    std::string why;
    {
        std::unique_ptr<WinHidPort> port = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log, &why, f.fn(), f.sleep());
        CHECK(port != nullptr);
        CHECK(!port->exclusive());
    }
    CHECK_EQ(f.exclusiveCalls, 1 + WinHidPort::kExclusiveRetries);
    CHECK_EQ(f.exclusiveCalls, 1);  // B3b F3: was 21
    CHECK_EQ(f.sharedCalls, 1);
    CHECK_EQ(f.sleptMs, 0);  // B3b F3: was 2000
    CHECK(log.has("ARBS-W 경고: 다른 프로그램이 IF2 를 쓰는 중(배타 열기 실패, 오류 32) -> 공유 모드로 엽니다. "
                  "Space Station·XHaptics·ApexSenseBridge·Steam 을 종료하세요"));
    CHECK(log.has("(배타 재시도 0회, 이 프로세스에 열린 IF2 포트 0개 - 0개면 ARBS-W 자신의 핸들이 아님)"));  // B3b F3: was "20회 모두 오류 32"
    CHECK(log.has("IF2 열림(공유"));
    CloseHandle(p.server);
    // another error: no retry, shared at once; shared also failing -> nullptr + reason
    CaptureLog log2;
    FakeOpen g;
    g.exclusiveErrors = {5};
    g.sharedError = 32;
    std::unique_ptr<WinHidPort> none = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log2, &why, g.fn(), g.sleep());
    CHECK(none == nullptr);
    CHECK_EQ(g.exclusiveCalls, 1);
    CHECK_EQ(g.sleptMs, 0);
    CHECK(log2.has("IF2 배타 열기 실패(오류 5) -> 공유 모드로 다시 시도"));
    CHECK_MSG(why == "IF2 열기 실패(공유 모드도 실패, 오류 32)", why);
}

// P4: detach -> re-open: with the real CreateFileW attempt on a temp file (no device). Our own exclusive port blocks the
// exclusive AND the shared open (so the field's "shared open succeeded" cannot be our own handle); after the port is
// destroyed the count is 0 and the same path opens exclusively again.
TEST(b2c_if2, p04_detach_then_reopen_exclusive) {
    const std::string dir = scratchDir();
    const std::wstring path = std::wstring(dir.begin(), dir.end()) + L"\\arbs-w-b2c-if2-stand-in.bin";
    {
        HANDLE c = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(c != INVALID_HANDLE_VALUE);
        CloseHandle(c);
    }
    const HidCandidate c = cand(path.c_str(), kApexV, kApexP, 0xFFA0);
    const int base = WinHidPort::openCount();
    int slept = 0;
    const WinHidPort::SleepFn noWait = [&slept](int ms) { slept += ms; };
    CaptureLog log;
    std::string why;
    std::unique_ptr<WinHidPort> first = WinHidPort::openWith(c, log, &why, &WinHidPort::createFileAttempt, noWait);
    CHECK(first != nullptr);
    CHECK(first->exclusive());
    CHECK_EQ(WinHidPort::openCount(), base + 1);
    CaptureLog busy;
    std::unique_ptr<WinHidPort> second = WinHidPort::openWith(c, busy, &why, &WinHidPort::createFileAttempt, noWait);
    CHECK(second == nullptr);
    CHECK_EQ(slept, 0);  // B3b F3: no retry wait (b2d: 2000)
    CHECK(busy.has("이 프로세스에 열린 IF2 포트 " + std::to_string(base + 1) + "개"));
    CHECK(busy.has("ms(배타 재시도 0회, 결과 실패)"));
    CHECK_MSG(why == "IF2 열기 실패(공유 모드도 실패, 오류 32)", why);
    first.reset();  // = Engine::detach -> port_.reset(): reader joined, CloseHandle, events closed
    CHECK_EQ(WinHidPort::openCount(), base);
    CHECK(log.has("IF2 닫힘"));
    CaptureLog again;
    slept = 0;
    std::unique_ptr<WinHidPort> third = WinHidPort::openWith(c, again, &why, &WinHidPort::createFileAttempt, noWait);
    CHECK(third != nullptr);
    CHECK(third->exclusive());
    CHECK_EQ(slept, 0);
    CHECK(again.has("IF2 열기 - 이 프로세스에 이미 열린 IF2 포트 " + std::to_string(base) + "개"));
    CHECK(!again.has("오류 32"));
    third.reset();
    CHECK_EQ(WinHidPort::openCount(), base);
    CHECK(DeleteFileW(path.c_str()) != 0);
}

// ------------------------------------------------------------------------------------------------ B3 (ARBS-W B2d)
// docs/analysis/B3.md section 3-1: B1-B2 gcpersist, R1-R4 exclusive retry window, F1-F5 first PCM, D1-D3 IF2 detach.
namespace {
int64_t msNs(int64_t ms) { return ms * 1000000LL; }
long long loggedStallMs(CaptureLog& log) {
    std::lock_guard<std::mutex> g(log.mu);
    const std::string key = "메인 스레드 정지 약 ";
    for (const std::string& l : log.lines) {
        const size_t at = l.find(key);
        if (at != std::string::npos) return std::stoll(l.substr(at + key.size()));
    }
    return -1;
}
int countLines(CaptureLog& log, const std::string& part) {
    std::lock_guard<std::mutex> g(log.mu);
    int n = 0;
    for (const std::string& l : log.lines)
        if (l.find(part) != std::string::npos) n++;
    return n;
}
}  // namespace

// B1: gcpersist truth table [잠정 D-B3-1] + the launch log line
TEST(b2d_persist, b1_truth_table_and_note) {
    CHECK(persistGamepads(true, true));    // multi + APEX-only -> 1 (new)
    CHECK(!persistGamepads(true, false));  // multi, APEX-only off -> 0 (upstream)
    CHECK(persistGamepads(false, false));  // single -> 1 (upstream)
    CHECK(persistGamepads(false, true));   // single + APEX-only -> 1 (upstream value too)
    // APEX-only off = upstream !multiController in every mode
    for (int m = 0; m < 2; m++) CHECK_EQ(persistGamepads(m == 1, false), m == 0);
    CHECK_MSG(persistGamepadsNote(true, true) == "ARBS-W: 시작 요청 gcpersist=1(APEX 단독 켬 -> 호스트 DualSense 유지·rebound(잠정 D-B3-1))",
              persistGamepadsNote(true, true));
    CHECK_MSG(persistGamepadsNote(true, false) == "ARBS-W: 시작 요청 gcpersist=0(APEX 단독 끔 = upstream 값(단일 모드면 1))",
              persistGamepadsNote(true, false));
    CHECK_MSG(persistGamepadsNote(false, false) == "ARBS-W: 시작 요청 gcpersist=1(APEX 단독 끔 = upstream 값(단일 모드면 1))",
              persistGamepadsNote(false, false));
    CHECK_MSG(persistGamepadsNote(false, true) == "ARBS-W: 시작 요청 gcpersist=1(APEX 단독 켬, 단일 모드 = upstream 값)",
              persistGamepadsNote(false, true));
}

// B2: APEX-only on: removal -> D1 -> reconnect sends the same packets as B2c H4/H5 (numbering rules unchanged; the host
// side rebound is not testable here - real-device host log "rebound to resumed client")
TEST(b2d_persist, b2_packet_order_same_as_b2c) {
    HostSim h = simOnly(true, true, 2, true);
    CHECK(persistGamepads(h.multi, h.apexOnly));
    h.add(kBuiltinJs, kBuiltinVid, kBuiltinPid);
    h.add(kApexJs, kApexV, kApexP);
    h.packets.clear();
    h.unplugExact(kApexJs);
    CHECK(h.remove(kApexJs).action == BA::Unbind);
    CHECK_MSG(h.all() == "gone0/m=0x0 mv1>0(D1) show100 A0:norm/m=0x1 in0:b=0x0/m=0x1 ", h.all());  // + current state once (B2c)
    h.packets.clear();
    const int again = kApexJs + 10;
    h.listExact = true;
    h.listExactIds = {again};
    CHECK(h.add(again, kApexV, kApexP).action == BA::BindExact);
    CHECK_MSG(h.all() == "mv0>1 hide100 R0/m=0x0 A0:apex/m=0x1 ", h.all());
}

// R1 (B3b F3 [잠정 D-B3b-2], was "exclusive after 19 retries"): no exclusive retry at all - error 32 once, the next
// exclusive try would succeed, but the port is opened shared at once and the stall line is still written
TEST(b2d_if2, r1_no_exclusive_retry) {
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    FakeOpen f;
    f.exclusiveErrors = {32, 0};
    f.handle = p.client;
    std::string why;
    {
        std::unique_ptr<WinHidPort> port = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log, &why, f.fn(), f.sleep());
        CHECK(port != nullptr);
        CHECK(!port->exclusive());
    }
    CHECK_EQ(WinHidPort::kExclusiveRetries, 0);  // B3b F3: was 20 (kRetryDelayMs 100 removed)
    CHECK_EQ(f.exclusiveCalls, 1);
    CHECK_EQ(f.sharedCalls, 1);
    CHECK_EQ(f.sleptMs, 0);
    CHECK(!log.has("배타 열기 성공(재시도"));
    CHECK(!log.has("다시 시도"));
    CHECK(log.has("ARBS-W: IF2 열기 - 메인 스레드 정지 약 "));
    CHECK(log.has("ms(배타 재시도 0회, 결과 공유)"));
    CHECK(log.has("다른 프로그램이 IF2 를 쓰는 중"));
    CloseHandle(p.server);
}

// R2 (B3b F3, was "twenty retries then shared"): error 32 -> one exclusive try, shared at once, one stall line;
// shared also failing -> "결과 실패"
TEST(b2d_if2, r2_busy_then_shared_no_wait) {
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    FakeOpen f;
    f.exclusiveErrors = {32};
    f.handle = p.client;
    std::string why;
    {
        std::unique_ptr<WinHidPort> port = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log, &why, f.fn(), f.sleep());
        CHECK(port != nullptr);
        CHECK(!port->exclusive());
    }
    CHECK_EQ(f.exclusiveCalls, 1);
    CHECK_EQ(f.sharedCalls, 1);
    CHECK_EQ(f.sleptMs, 0);
    CHECK(!log.has("모두 오류 32"));
    CHECK(log.has("배타 재시도 0회, 이 프로세스에 열린 IF2 포트"));
    CHECK(log.has("ms(배타 재시도 0회, 결과 공유)"));
    CHECK_EQ(countLines(log, "메인 스레드 정지 약"), 1);
    CloseHandle(p.server);
    CaptureLog log2;
    FakeOpen g;
    g.exclusiveErrors = {32};
    g.sharedError = 5;
    CHECK(WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log2, &why, g.fn(), g.sleep()) == nullptr);
    CHECK_EQ(g.exclusiveCalls, 1);
    CHECK_EQ(g.sleptMs, 0);
    CHECK(log2.has("ms(배타 재시도 0회, 결과 실패)"));
    CHECK_EQ(countLines(log2, "메인 스레드 정지 약"), 1);
    CHECK_MSG(why == "IF2 열기 실패(공유 모드도 실패, 오류 5)", why);
}

// R3: not error 32 -> no retry, no stall line; first try exclusive -> no stall line
TEST(b2d_if2, r3_no_retry_no_stall_line) {
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    FakeOpen f;
    f.exclusiveErrors = {5};
    f.handle = p.client;
    std::string why;
    {
        std::unique_ptr<WinHidPort> port = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log, &why, f.fn(), f.sleep());
        CHECK(port != nullptr);
        CHECK(!port->exclusive());
    }
    CHECK_EQ(f.exclusiveCalls, 1);
    CHECK_EQ(f.sleptMs, 0);
    CHECK(!log.has("메인 스레드 정지"));
    CloseHandle(p.server);
    PipePair q = makePipe();
    CHECK(q.client != INVALID_HANDLE_VALUE);
    CaptureLog log2;
    FakeOpen g;
    g.exclusiveErrors = {0};
    g.handle = q.client;
    {
        std::unique_ptr<WinHidPort> port = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log2, &why, g.fn(), g.sleep());
        CHECK(port != nullptr);
        CHECK(port->exclusive());
    }
    CHECK(!log2.has("메인 스레드 정지"));
    CloseHandle(q.server);
}

// R4 (B3b F3): the stall line is the real (steady clock) blocked time of the whole open (here: a slow shared open),
// and the sleep seam is never called
TEST(b2d_if2, r4_stall_line_is_real_time) {
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    int exclusiveCalls = 0, sharedCalls = 0, planned = 0;
    const WinHidPort::OpenFn slowShared = [&](const std::wstring&, bool exclusive) {
        WinHidPort::OpenAttempt a;
        if (exclusive) {
            exclusiveCalls++;
            a.error = 32;
        } else {
            sharedCalls++;
            Sleep(60);  // the real CreateFileW time is what the line must show
            a.handle = p.client;
        }
        return a;
    };
    const WinHidPort::SleepFn countSleep = [&planned](int ms) { planned += ms; };
    std::string why;
    {
        std::unique_ptr<WinHidPort> port = WinHidPort::openWith(cand(L"x", kApexV, kApexP, 0xFFA0), log, &why, slowShared, countSleep);
        CHECK(port != nullptr);
        CHECK(!port->exclusive());
    }
    CHECK_EQ(planned, 0);
    CHECK_EQ(exclusiveCalls, 1);
    CHECK_EQ(sharedCalls, 1);
    const long long ms = loggedStallMs(log);
    CHECK_MSG(ms >= 55 && ms < 1000, std::to_string(ms));
    CHECK(log.has("ms(배타 재시도 0회, 결과 공유)"));
    CloseHandle(p.server);
    CHECK_MSG(openStallLine(1234, 7, "공유") == "ARBS-W: IF2 열기 - 메인 스레드 정지 약 1234 ms(배타 재시도 7회, 결과 공유)",
              openStallLine(1234, 7, "공유"));
    CHECK_MSG(bindStallLine(2105, 3, 2001) == "ARBS-W: APEX 묶기 메인 스레드 2105 ms(이전 포트 정리 3 ms + IF2 열기 2001 ms)",
              bindStallLine(2105, 3, 2001));
}

// F1: arm -> first PCM of that number 1234 ms later -> one line; the second packet gives nothing
TEST(b2d_pcm, f1_first_pcm_once) {
    PcmWait w;
    PcmWait::Result r;
    CHECK(!w.onPcm(msNs(5), 0, &r));  // not armed
    w.arm(msNs(1000), 0, PcmWait::Kind::AfterReconnect);
    CHECK(w.armed());
    CHECK(w.onPcm(msNs(2234), 0, &r));
    CHECK_EQ(r.ms, 1234ll);
    CHECK_EQ(r.index, 0);
    CHECK(r.kind == PcmWait::Kind::AfterReconnect);
    CHECK(!w.armed());
    CHECK(!w.onPcm(msNs(2240), 0, &r));
    CHECK_MSG(pcmWaitLine(r) == "ARBS-W: 재연결 후 첫 PCM 1234 ms(APEX 도착 보고 -> 첫 PCM, 번호 0)", pcmWaitLine(r));
    PcmWait::Result none;
    CHECK(!w.cancel(msNs(3000), &none));  // already fired -> no "없음" line
}

// F2: PCM for another number first -> only the armed number ends the wait; a second arm replaces the first
TEST(b2d_pcm, f2_other_number_ignored) {
    PcmWait w;
    PcmWait::Result r;
    w.arm(msNs(0), 0, PcmWait::Kind::AfterReconnect);
    CHECK(!w.onPcm(msNs(10), 1, &r));
    CHECK(w.armed());
    CHECK(w.onPcm(msNs(20), 0, &r));
    CHECK_EQ(r.ms, 20ll);
    CHECK_EQ(r.index, 0);
    w.arm(msNs(100), 0, PcmWait::Kind::AfterReconnect);
    w.arm(msNs(150), 1, PcmWait::Kind::AfterReconnect);
    CHECK(!w.onPcm(msNs(160), 0, &r));
    CHECK(w.onPcm(msNs(170), 1, &r));
    CHECK_EQ(r.ms, 20ll);
    CHECK_EQ(r.index, 1);
}

// F3: unbind / session end before the first PCM -> "없음" line with the time waited
TEST(b2d_pcm, f3_cancel_none_line) {
    PcmWait w;
    PcmWait::Result r;
    CHECK(!w.cancel(msNs(1), &r));
    w.arm(msNs(500), 0, PcmWait::Kind::AfterReconnect);
    CHECK(w.cancel(msNs(12500), &r));
    CHECK_EQ(r.ms, 12000ll);
    CHECK(!w.armed());
    CHECK(!w.onPcm(msNs(12600), 0, &r));
    CHECK(!w.cancel(msNs(12700), &r));
    PcmWait::Result n;
    n.ms = 12000;
    n.index = 0;
    n.kind = PcmWait::Kind::AfterReconnect;
    CHECK_MSG(pcmWaitNoneLine(n, "SDL 제거") == "ARBS-W: 재연결 후 첫 PCM 없음(12000 ms 기다림, 번호 0, SDL 제거)", pcmWaitNoneLine(n, "SDL 제거"));
    n.kind = PcmWait::Kind::AfterConnect;
    CHECK_MSG(pcmWaitNoneLine(n, "세션 종료") == "ARBS-W: 접속 후 첫 PCM 없음(12000 ms 기다림, 번호 0, 세션 종료)", pcmWaitNoneLine(n, "세션 종료"));
}

// F4: first arrival of the session = "접속 후", later arrivals = "재연결 후"
TEST(b2d_pcm, f4_connect_vs_reconnect) {
    CHECK(PcmWait::kindFor(0) == PcmWait::Kind::AfterConnect);
    CHECK(PcmWait::kindFor(1) == PcmWait::Kind::AfterReconnect);
    CHECK(PcmWait::kindFor(7) == PcmWait::Kind::AfterReconnect);
    PcmWait w;
    PcmWait::Result r;
    int arrivals = 0;  // = Impl::apexArrivalsThisSession
    w.arm(msNs(0), 0, PcmWait::kindFor(arrivals++));
    CHECK(w.onPcm(msNs(21000), 0, &r));
    CHECK_MSG(pcmWaitLine(r) == "ARBS-W: 접속 후 첫 PCM 21000 ms(번호 0)", pcmWaitLine(r));
    w.arm(msNs(30000), 0, PcmWait::kindFor(arrivals++));
    CHECK(w.onPcm(msNs(30800), 0, &r));
    CHECK_MSG(pcmWaitLine(r) == "ARBS-W: 재연결 후 첫 PCM 800 ms(APEX 도착 보고 -> 첫 PCM, 번호 0)", pcmWaitLine(r));
}

// F5: arm on this thread, onPcm hammered from two other threads (armed number + another number): one record per arm
TEST(b2d_pcm, f5_threads_one_record_per_arm) {
    PcmWait w;
    std::atomic<bool> stop{false};
    std::atomic<int> fired{0}, wrong{0};
    std::thread rx([&] {
        PcmWait::Result r;
        while (!stop.load()) {
            if (w.onPcm(msNs(1), 0, &r)) {
                if (r.index != 0) wrong++;
                fired++;
            }
        }
    });
    std::thread other([&] {
        PcmWait::Result r;
        while (!stop.load())
            if (w.onPcm(msNs(1), 3, &r)) wrong++;
    });
    bool allFired = true;
    for (int i = 0; i < 1000; i++) {
        w.arm(0, 0, PcmWait::Kind::AfterReconnect);
        // yield-spin (waitFor sleeps 2 ms = ~15 ms on Windows; 1,000 rounds would take ~15 s)
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (fired.load() != i + 1 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
        if (fired.load() != i + 1) {
            allFired = false;
            break;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    rx.join();
    other.join();
    CHECK(allFired);
    CHECK_EQ(fired.load(), 1000);
    CHECK_EQ(wrong.load(), 0);
}

// D1: IF2 detach -> 5 s notice once / SDL removal first -> no notice + "SDL 제거까지 N ms"
TEST(b2d_detach, d1_notice_and_removal_time) {
    If2DetachWatch w;
    CHECK(!w.onDetach(1, msNs(0)));  // not armed
    const int gen = w.arm();
    CHECK(!w.onTick(msNs(10000)));  // armed, not detached
    CHECK(w.onDetach(gen, msNs(1000)));
    CHECK(!w.onDetach(gen, msNs(1100)));  // once
    CHECK(w.detached());
    CHECK(!w.onTick(msNs(5999)));  // 4,999 ms
    CHECK(w.onTick(msNs(6000)));   // 5,000 ms
    CHECK(!w.onTick(msNs(7000)));  // once
    CHECK_EQ(w.onSdlRemoved(msNs(9500)), 8500ll);
    CHECK(!w.detached());
    CHECK_EQ(w.onSdlRemoved(msNs(9600)), -1ll);
    // SDL removal before 5 s -> no notice
    const int g2 = w.arm();
    CHECK(w.onDetach(g2, msNs(20000)));
    CHECK(!w.onTick(msNs(21000)));
    CHECK_EQ(w.onSdlRemoved(msNs(21234)), 1234ll);
    CHECK(!w.onTick(msNs(30000)));
    // no detach before the SDL removal -> -1 (no line)
    w.arm();
    CHECK_EQ(w.onSdlRemoved(msNs(40000)), -1ll);
    CHECK(If2DetachWatch::kNoticeMs == 5000);
    CHECK_MSG(detachRemovedLine(8500) == "ARBS-W: IF2 분리 후 SDL 제거까지 8500 ms", detachRemovedLine(8500));
    CHECK_MSG(detachNoticeLine() == "ARBS-W: IF2 분리 5초 경과 - SDL 제거 아직 없음(숨김 유지 D3; 내장 패드를 쓰려면 동글을 뽑거나 APEX 단독 끄기)",
              detachNoticeLine());
}

// D2: detached + APEX SDL input for more than 2 s -> one warning; input before the detach does not count
TEST(b2d_detach, d2_input_while_detached_warns_once) {
    If2DetachWatch w;
    const int gen = w.arm();
    CHECK(!w.onApexInput(msNs(0)));
    CHECK(!w.onApexInput(msNs(5000)));  // not detached: never
    CHECK(w.onDetach(gen, msNs(6000)));
    CHECK(!w.onApexInput(msNs(6100)));  // first input after the detach
    CHECK(!w.onApexInput(msNs(8100)));  // 2,000 ms: not "more than"
    CHECK(w.onApexInput(msNs(8101)));   // 2,001 ms
    CHECK(!w.onApexInput(msNs(9000)));  // once
    CHECK(If2DetachWatch::kInputWarnMs == 2000);
    CHECK_MSG(detachInputWarnLine() == "ARBS-W 경고: IF2 분리 상태인데 APEX 입력이 계속 들어옴 - 진동 경로 끊김(동글 다시 꽂기)",
              detachInputWarnLine());
    // a new bind clears the state
    const int g2 = w.arm();
    CHECK(!w.detached());
    CHECK(w.onDetach(g2, msNs(10000)));
    CHECK(!w.onApexInput(msNs(10001)));
    CHECK(w.onApexInput(msNs(12100)));
}

// D3: stale generations (old port after re-bind / unbind) are ignored; the real port calls its listener exactly once
TEST(b2d_detach, d3_stale_generation_and_port_listener) {
    If2DetachWatch w;
    const int g1 = w.arm();
    const int g2 = w.arm();
    CHECK(g1 != g2);
    CHECK(!w.onDetach(g1, msNs(1)));
    CHECK(!w.detached());
    w.disarm();
    CHECK(!w.onDetach(g2, msNs(2)));
    CHECK(!w.onTick(msNs(100000)));
    // port over a pipe: the other end goes away -> listener once (reader thread); set after the detach -> called at once
    PipePair p = makePipe();
    CHECK(p.client != INVALID_HANDLE_VALUE);
    CaptureLog log;
    std::atomic<int> calls{0};
    {
        WinHidPort port(p.client, 33, 33, log, true);
        const int gen = w.arm();
        port.setDisconnectListener([&calls, &w, gen] {
            calls++;
            w.onDetach(gen, msNs(50));
        });
        CloseHandle(p.server);
        p.server = INVALID_HANDLE_VALUE;
        CHECK(waitFor([&] { return calls.load() == 1; }, 2000));
        uint8_t frame[32] = {};
        CHECK(port.writeFrame(frame, 100) == arbs::IoResult::Disconnected);
        CHECK(w.detached());
        std::atomic<int> late{0};
        port.setDisconnectListener([&late] { late++; });
        CHECK_EQ(late.load(), 1);
    }
    CHECK_EQ(calls.load(), 1);
    CHECK(waitFor([&] { return log.has("IF2 분리 감지"); }, 2000));
}

// ------------------------------------------------------------------------------------------------ B3b (ARBS-W b2e)
// docs/analysis/B3b.md: F1-T1..T3 (MC merge rule, provisional decision D-B3b-1), F2 D1-D3 diagnostics (D-B3b-3).
namespace {

// F1 wire model: one moonlight-common-c input queue (InputStream.c sendControllerEventInternal :1144-1251,
// sendControllerArrivalEvent :1559-1582). current[n] = queued MC holder of number n that a later MC may update in place
// (cleared when the send thread takes the queue). An arrival is its own queue item and is followed by an MC.
struct WirePkt {
    bool arrival = false;
    int index = 0;
    uint16_t mask = 0, buttons = 0, caps = 0;
    uint8_t lt = 0, rt = 0;
};
struct WireModel {
    bool maskRule = true;  // true = ArbsMcBatchCompatible (b2e), false = upstream rule without the mask (control run)
    std::vector<WirePkt> queue;
    int current[16];
    WireModel() { std::fill(current, current + 16, -1); }
    void mc(int index, uint16_t mask, uint16_t buttons = 0, uint8_t lt = 0, uint8_t rt = 0) {
        int h = current[index];
        if (h >= 0) {
            const WirePkt& q = queue[static_cast<size_t>(h)];
            const int ok = maskRule ? ArbsMcBatchCompatible(q.buttons, 0, q.lt, q.rt, q.mask, buttons, 0, lt, rt, mask)
                                    : ArbsMcBatchFieldsCompatible(q.buttons, 0, q.lt, q.rt, buttons, 0, lt, rt);
            if (!ok) h = -1;
        }
        if (h < 0) {
            WirePkt p;
            p.index = index;
            p.buttons = buttons;
            queue.push_back(p);
            h = static_cast<int>(queue.size()) - 1;
            current[index] = h;
        }
        WirePkt& w = queue[static_cast<size_t>(h)];  // InputStream.c :1235: mask/triggers/sticks always overwritten
        w.mask = mask;
        w.lt = lt;
        w.rt = rt;
    }
    void arrival(int index, uint16_t mask, uint16_t caps) {
        WirePkt p;
        p.arrival = true;
        p.index = index;
        p.mask = mask;
        p.caps = caps;
        queue.push_back(p);
        mc(index, mask);  // "Send a MC event just in case the host software doesn't support arrival events"
    }
    // send thread takes everything that is queued (worst case: only after step 3)
    std::vector<WirePkt> drain() {
        std::fill(current, current + 16, -1);
        std::vector<WirePkt> out;
        out.swap(queue);
        return out;
    }
};

// F1 host model (Vibepollo 2.18.9 / ARBS-H): arrival on an allocated number -> "already allocated" + return
// (H:src/input.cpp:1263-1266); MC whose mask lacks a number -> reset (1639-1646), with gcpersist the DualSense is kept
// ("retained", dualsense_usbip_gamepad.cpp:605-606) and the next arrival re-binds it with that arrival's caps
// (:452/:469 waveform_capable = caps & PCM); MC with the bit for an unallocated number -> legacy creation without
// caps (input.cpp:1659). PCM is forwarded only for a waveform-capable pad (dualsense_usbip_gamepad.cpp:281-290).
struct HostPadModel {
    struct Pad {
        bool allocated = false, retained = false, waveform = false;
    };
    Pad pads[16];
    std::vector<std::string> log;
    void apply(const WirePkt& p) {
        Pad& g = pads[p.index];
        if (p.arrival) {
            if (g.allocated) {
                log.push_back("ControllerNumber already allocated");
                return;
            }
            log.push_back(g.retained ? "rebound" : "created");
            g.allocated = true;
            g.retained = false;
            g.waveform = (p.caps & kCapHapticsPcm) != 0;
            return;
        }
        for (int i = 0; i < 16; i++) {
            if (!(p.mask & (1u << i)) && pads[i].allocated) {
                pads[i].allocated = false;
                pads[i].retained = true;
                log.push_back("retained");
            }
        }
        if ((p.mask & (1u << p.index)) && !g.allocated) {
            g.allocated = true;
            g.retained = false;
            g.waveform = false;
            log.push_back("legacy created");
        }
    }
    std::string pcm(int index) const {
        return pads[index].allocated && pads[index].waveform ? "PCM frame accepted" : "PCM not forwarded: controller lacks waveform capability";
    }
    bool has(const std::string& s) const { return std::find(log.begin(), log.end(), s) != log.end(); }
};

constexpr uint16_t kBuiltinCaps = 0x43;  // EA-B3-U-01: built-in pad arrival caps

// F1-T2/T3 scenario: session starts without the APEX (built-in pad created number 0 with caps 0x43), then the APEX
// arrives: the real planArrival steps (Remove(0, 0x0) + APEX Arrival(0, 0x1)) go through the wire model and the send
// thread takes the queue only after step 3 (worst case). [pendingInput] = the built-in pad's last input MC is still queued.
HostPadModel runF1Scenario(bool maskRule, bool multi, bool pendingInput) {
    HostPadModel host;
    WireModel wire;
    wire.maskRule = maskRule;
    HostLedger led;
    const int builtinJs = 3, apexJs = 9;
    // connect: built-in arrival + its MC, sent
    std::vector<HostStep> first = planArrival(led, multi, true, 0, false, builtinJs, 0x1);
    for (const HostStep& s : first) led.sent(s, builtinJs);
    wire.arrival(0, 0x1, kBuiltinCaps);
    for (const WirePkt& p : wire.drain()) host.apply(p);
    if (pendingInput) wire.mc(0, 0x1, 0, 0, 0);
    // APEX arrives (APEX-only: the built-in pad becomes hidden, mask = APEX bit only)
    const std::vector<HostStep> steps = planArrival(led, multi, true, 0, true, apexJs, 0x1);
    for (const HostStep& s : steps) {
        if (s.kind == HostStep::Kind::Remove) wire.mc(s.index, s.mask);
        else wire.arrival(s.index, s.mask, static_cast<uint16_t>(apexCaps(kBuiltinCaps)));
        led.sent(s, apexJs);
    }
    for (const WirePkt& p : wire.drain()) host.apply(p);
    return host;
}

}  // namespace

// F1-T1: merge rule table (the function InputStream.c calls)
TEST(b3b_f1, t1_merge_rule) {
    // same buttons, different mask -> no merge (new); same buttons, same mask -> merge; different buttons -> no merge (upstream)
    CHECK(!ArbsMcBatchCompatible(0, 0, 0, 0, 0x0, 0, 0, 0, 0, 0x1));
    CHECK(!ArbsMcBatchCompatible(0, 0, 0, 0, 0x3, 0, 0, 0, 0, 0x1));
    CHECK(ArbsMcBatchCompatible(0, 0, 0, 0, 0x1, 0, 0, 0, 0, 0x1));
    CHECK(ArbsMcBatchCompatible(0x1000, 0, 20, 30, 0x1, 0x1000, 0, 40, 60, 0x1));  // travel inside a range still merges
    CHECK(!ArbsMcBatchCompatible(0x1000, 0, 0, 0, 0x1, 0x0000, 0, 0, 0, 0x1));
    CHECK(!ArbsMcBatchCompatible(0, 0x1, 0, 0, 0x1, 0, 0x0, 0, 0, 0x1));
    // upstream trigger edges unchanged: rest <-> pressed, crossing 0xF0
    CHECK(!ArbsMcBatchCompatible(0, 0, 0, 0, 0x1, 0, 0, 1, 0, 0x1));
    CHECK(!ArbsMcBatchCompatible(0, 0, 0, 5, 0x1, 0, 0, 0, 0, 0x1));
    CHECK(!ArbsMcBatchCompatible(0, 0, 0xEF, 0, 0x1, 0, 0, 0xF0, 0, 0x1));
    CHECK(!ArbsMcBatchCompatible(0, 0, 0, 0xF0, 0x1, 0, 0, 0, 0xEF, 0x1));
    // the upstream part alone ignores the mask (control rule for F1-T3)
    CHECK(ArbsMcBatchFieldsCompatible(0, 0, 0, 0, 0, 0, 0, 0));
    CHECK(!ArbsMcBatchFieldsCompatible(1, 0, 0, 0, 0, 0, 0, 0));
    // exhaustive equivalence with the upstream expression when the masks are equal (buttons x trigger edges)
    const uint8_t trig[] = {0, 1, 0xEF, 0xF0, 0xFF};
    int checked = 0;
    const uint16_t btn[] = {0, 1, 0x8000};
    for (uint16_t hb : btn)
        for (uint16_t nb : btn)
            for (uint8_t hl : trig)
                for (uint8_t nl : trig)
                    for (uint8_t hr : trig)
                        for (uint8_t nr : trig) {
                            const bool upstream = !(hb != nb || (hl == 0) != (nl == 0) || (hr == 0) != (nr == 0) ||
                                                    (hl >= 0xF0) != (nl >= 0xF0) || (hr >= 0xF0) != (nr >= 0xF0));
                            CHECK_EQ(ArbsMcBatchCompatible(hb, 0, hl, hr, 0x1, nb, 0, nl, nr, 0x1) != 0, upstream);
                            checked++;
                        }
    CHECK_EQ(checked, 9 * 625);
}

// F1-T2: start with the built-in pad (0x43) -> APEX arrives -> first PCM accepted (b2e rule), worst-case send timing
TEST(b3b_f1, t2_builtin_then_apex_first_pcm_accepted) {
    CHECK((apexCaps(kBuiltinCaps) & kCapHapticsPcm) != 0);
    for (int multi = 0; multi < 2; multi++) {
        for (int pending = 0; pending < 2; pending++) {
            const HostPadModel h = runF1Scenario(true, multi == 1, pending == 1);
            const std::string tag = arbs::jc::fmt("multi=%d pending=%d", multi, pending);
            CHECK_MSG(!h.has("ControllerNumber already allocated"), tag);
            CHECK_MSG(h.has("retained") && h.has("rebound"), tag);
            CHECK_MSG(h.pads[0].allocated && h.pads[0].waveform, tag);
            CHECK_MSG(h.pcm(0) == "PCM frame accepted", tag);
        }
    }
    // wire order on channel 0 (multi, worst case): removal MC 0x0, APEX arrival, MC 0x1 = three separate packets
    WireModel w;
    w.mc(0, 0x0);
    w.arrival(0, 0x1, 0x8043);
    const std::vector<WirePkt> q = w.drain();
    CHECK_EQ(q.size(), static_cast<size_t>(3));
    CHECK(!q[0].arrival && q[0].mask == 0x0);
    CHECK(q[1].arrival && q[1].mask == 0x1);
    CHECK(!q[2].arrival && q[2].mask == 0x1);
}

// F1-T3: control run - the old rule (no mask comparison) reproduces the field failure (EA-B3-U-01 / L1:597)
TEST(b3b_f1, t3_control_old_rule_fails) {
    for (int multi = 0; multi < 2; multi++) {
        for (int pending = 0; pending < 2; pending++) {
            const HostPadModel h = runF1Scenario(false, multi == 1, pending == 1);
            const std::string tag = arbs::jc::fmt("multi=%d pending=%d", multi, pending);
            CHECK_MSG(h.has("ControllerNumber already allocated"), tag);
            CHECK_MSG(!h.has("rebound"), tag);
            CHECK_MSG(h.pads[0].allocated && !h.pads[0].waveform, tag);
            CHECK_MSG(h.pcm(0) == "PCM not forwarded: controller lacks waveform capability", tag);
        }
    }
    WireModel w;
    w.maskRule = false;
    w.mc(0, 0x0);
    w.arrival(0, 0x1, 0x8043);
    const std::vector<WirePkt> q = w.drain();
    CHECK_EQ(q.size(), static_cast<size_t>(2));  // the removal MC was overwritten with mask 0x1
    CHECK(!q[0].arrival && q[0].mask == 0x1);
}

// F2 D1-D3 line formats + frame command classifier
TEST(b3b_f2, d_lines_and_frame_command) {
    const arbs::protocol::Frame gi = arbs::protocol::buildGetInfo();
    const arbs::protocol::Frame en = arbs::protocol::gripRealtimeEnable();
    CHECK_EQ(apexFrameCommand(gi.data()), 0x01);
    CHECK_EQ(apexFrameCommand(en.data()), 0x53);
    uint8_t bad[32] = {0x57, 0xA5, 0x53};
    CHECK_EQ(apexFrameCommand(bad), -1);
    CHECK_MSG(openToDetachLine(11, 0, 0) == "ARBS-W: IF2 열기 후 분리까지 11 ms(GetInfo 송신 전, 0x53 송신 전)", openToDetachLine(11, 0, 0));
    CHECK_MSG(openToDetachLine(3500, 2, 1) == "ARBS-W: IF2 열기 후 분리까지 3500 ms(GetInfo 송신 후 2회, 0x53 송신 후 1회)",
              openToDetachLine(3500, 2, 1));
    CHECK_MSG(unbindPortsLine(0) == "ARBS-W: 엔진 분리 뒤 이 프로세스에 열린 IF2 포트 0개", unbindPortsLine(0));
    CHECK_MSG(unbindPortsLine(1) ==
                  "ARBS-W 경고: 엔진 분리 뒤 이 프로세스에 열린 IF2 포트 1개 - 엔진 스레드가 1.5초 안에 끝나지 않아 IF2 핸들이 남음"
                  "(다음 연결 때 정리; 동글 재인식을 막을 수 있음, F2 진단)",
              unbindPortsLine(1));
    const std::string sum = "HID 인터페이스 12개 중 APEX 동글 경로 0개(열림 0), IF2(UsagePage 0xFFA0) 0개";
    CHECK_MSG(hidRecheckLine(5000, sum, 0) ==
                  "ARBS-W: IF2 분리 5초 뒤 SDL 에 37d7/2502 없음 - Windows HID 인터페이스 12개 중 APEX 동글 경로 0개(열림 0), "
                  "IF2(UsagePage 0xFFA0) 0개, 이 프로세스 IF2 포트 0개(F2 진단)",
              hidRecheckLine(5000, sum, 0));
    CHECK(hidRecheckLine(30000, sum, 1).find("IF2 분리 30초 뒤") != std::string::npos);
}

// F2-D3: checkpoints 5 s and 30 s once each per start; stop cancels; a new start restarts
TEST(b3b_f2, d3_recheck_watch) {
    HidRecheckWatch w;
    CHECK_EQ(w.onTick(msNs(100000)), 0ll);  // not started
    w.start(msNs(1000));
    CHECK(w.active());
    CHECK_EQ(w.onTick(msNs(5999)), 0ll);
    CHECK_EQ(w.onTick(msNs(6000)), 5000ll);
    CHECK_EQ(w.onTick(msNs(7000)), 0ll);  // once
    CHECK_EQ(w.onTick(msNs(30999)), 0ll);
    CHECK_EQ(w.onTick(msNs(31000)), 30000ll);
    CHECK(!w.active());
    CHECK_EQ(w.onTick(msNs(60000)), 0ll);
    // stop (new bind / session end) before 5 s -> nothing
    w.start(msNs(100000));
    w.stop();
    CHECK_EQ(w.onTick(msNs(200000)), 0ll);
    // timer stalled past 30 s -> one scan only (30 s)
    w.start(msNs(300000));
    CHECK_EQ(w.onTick(msNs(340000)), 30000ll);
    CHECK_EQ(w.onTick(msNs(341000)), 0ll);
    // restart after the first checkpoint
    w.start(msNs(400000));
    CHECK_EQ(w.onTick(msNs(405000)), 5000ll);
    w.start(msNs(406000));
    CHECK_EQ(w.onTick(msNs(411000)), 5000ll);
    CHECK(HidRecheckWatch::kFirstMs == 5000 && HidRecheckWatch::kSecondMs == 30000);
}

// F2-D1: the real port over a pipe - GetInfo + 0x53 written, then the other end goes away -> one D1 line after the
// "IF2 분리 감지" line; a port that wrote nothing reports "송신 전"
TEST(b3b_f2, d1_open_to_detach_over_pipe) {
    {
        PipePair p = makePipe();
        CHECK(p.client != INVALID_HANDLE_VALUE);
        CaptureLog log;
        {
            WinHidPort port(p.client, 33, 33, log, true);
            const arbs::protocol::Frame gi = arbs::protocol::buildGetInfo();
            const arbs::protocol::Frame en = arbs::protocol::gripRealtimeEnable();
            CHECK(port.writeFrame(gi.data(), 100) == arbs::IoResult::Ok);
            CHECK(port.writeFrame(en.data(), 100) == arbs::IoResult::Ok);
            uint8_t rt[32] = {0x5A, 0xA5, 0x57};
            CHECK(port.writeFrame(rt, 100) == arbs::IoResult::Ok);
            CHECK_EQ(port.stats().getInfoWrites, 1ull);
            CHECK_EQ(port.stats().motorRouteWrites, 1ull);
            CHECK_EQ(port.stats().writes, 3ull);
            CloseHandle(p.server);
            p.server = INVALID_HANDLE_VALUE;
            CHECK(waitFor([&] { return log.has("IF2 열기 후 분리까지"); }, 2000));
        }
        CHECK(log.has("ms(GetInfo 송신 후 1회, 0x53 송신 후 1회)"));
        CHECK_EQ(countLines(log, "IF2 열기 후 분리까지"), 1);
    }
    {
        PipePair p = makePipe();
        CHECK(p.client != INVALID_HANDLE_VALUE);
        CaptureLog log;
        {
            WinHidPort port(p.client, 33, 33, log, false);
            CloseHandle(p.server);
            p.server = INVALID_HANDLE_VALUE;
            CHECK(waitFor([&] { return log.has("IF2 열기 후 분리까지"); }, 2000));
        }
        CHECK(log.has("ms(GetInfo 송신 전, 0x53 송신 전)"));
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
