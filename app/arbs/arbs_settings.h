// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W settings (B2 D9, CC_TASK_B2 3-4). Same items, allowed values and defaults as ARBS-A ArbsSettings.java:57-69
// (ASB defaults). Pure C++ (no Qt) so that the normalisation rules are unit-tested; StreamingPreferences stores them.
// A stored value outside the allowed list is replaced by the ASB default when read (ARBS-A A1-4b lesson).
#pragma once

#include <cstddef>

namespace arbsw {

struct ArbsSettings {
    int amp = 127;              // amplitude cap (0..127). list_arbs_apex_amp
    double gain = 1.0;          // gain G. list_arbs_apex_gain
    int gatePct = 0;            // noise gate %FS, 0 = off. list_arbs_gate
    int silence = 1;            // silence handling mode 1..4. list_arbs_silence_mode
    int curve = 0;              // 0 = ASB curve, 1 = tanh. list_arbs_curve
    int taps = 321;             // FIR taps. list_arbs_filter
    int jitterFixedMs = 0;      // 0 = auto (p95 + 4 ms). list_arbs_jitter_fixed
    bool trigger = true;        // trigger haptics. checkbox_arbs_trigger
    int triggerStrength = 100;  // %. list_arbs_trigger_strength
    bool verbose = false;       // keep 1 s lines after 20 MB. checkbox_arbs_verbose_log
};

namespace settings {

constexpr int kAmpValues[] = {0, 10, 20, 30, 40, 50, 60, 80, 100, 127};
constexpr double kGainValues[] = {1.0, 2.0, 3.0, 4.0, 6.0, 8.0};
constexpr int kGateValues[] = {0, 1, 2, 4};
constexpr int kSilenceValues[] = {1, 2, 3, 4};
constexpr int kCurveValues[] = {0, 1};
constexpr int kTapsValues[] = {161, 321, 641};
constexpr int kJitterValues[] = {0, 16, 24, 32, 48, 64, 80};
constexpr int kStrengthValues[] = {0, 20, 40, 60, 80, 100};

template <size_t N>
inline int normalizeInt(int v, const int (&allowed)[N], int def) {
    for (size_t i = 0; i < N; i++) {
        if (allowed[i] == v) return v;
    }
    return def;
}

template <size_t N>
inline double normalizeDouble(double v, const double (&allowed)[N], double def) {
    for (size_t i = 0; i < N; i++) {
        const double d = allowed[i] - v;
        if (d < 1e-6 && d > -1e-6) return allowed[i];
    }
    return def;
}

// Returns a copy where every out-of-list value is the ASB default. [changed] (optional) = number of replaced fields.
inline ArbsSettings normalize(const ArbsSettings& in, int* changed = nullptr) {
    const ArbsSettings d;
    ArbsSettings o = in;
    o.amp = normalizeInt(in.amp, kAmpValues, d.amp);
    o.gain = normalizeDouble(in.gain, kGainValues, d.gain);
    o.gatePct = normalizeInt(in.gatePct, kGateValues, d.gatePct);
    o.silence = normalizeInt(in.silence, kSilenceValues, d.silence);
    o.curve = normalizeInt(in.curve, kCurveValues, d.curve);
    o.taps = normalizeInt(in.taps, kTapsValues, d.taps);
    o.jitterFixedMs = normalizeInt(in.jitterFixedMs, kJitterValues, d.jitterFixedMs);
    o.triggerStrength = normalizeInt(in.triggerStrength, kStrengthValues, d.triggerStrength);
    if (changed != nullptr) {
        *changed = (o.amp != in.amp) + (o.gain != in.gain) + (o.gatePct != in.gatePct) + (o.silence != in.silence) +
                   (o.curve != in.curve) + (o.taps != in.taps) + (o.jitterFixedMs != in.jitterFixedMs) +
                   (o.triggerStrength != in.triggerStrength);
    }
    return o;
}

}  // namespace settings
}  // namespace arbsw
