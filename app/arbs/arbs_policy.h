// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W provisional decisions (B2 D8). Every item here is a design recommendation that still waits for the planning
// owner's ruling (docs/analysis/B2_DESIGN.md section 10, docs/research/B2_BUILD.md "잠정 결정"). Changing one of
// them is a one-line edit here; each is read in exactly one place (grep the constant name).
#pragma once

namespace arbsw {
namespace policy {

// [잠정 1] rumble for the APEX goes to ARBS Core only (grip synthesis, same as ARBS-A). false = when the Core engine is
// not READY (UNSUPPORTED / probing), hand the rumble to the normal SDL (XInput) path instead.
// Used in: ArbsBridge::consumeRumble (arbs_bridge.cpp).
constexpr bool kRumbleCoreOnly = true;

// [잠정 4] Minimising the stream window pauses the APEX output (stop sequence) and restoring resumes it
// (ARBS-A onPause/onResume). Focus loss alone does nothing. false = minimise has no effect on the APEX.
// Used in: ArbsBridge::onWindowMinimized / onWindowRestored.
constexpr bool kPauseOnMinimize = true;

// [잠정 2] (±1 LSB tolerance) and [잠정 3] (Core size) have no runtime switch: they only affect the Core golden tests
// (core/arbs-core/tests, strict 0 today) and the size estimate.

}  // namespace policy
}  // namespace arbsw
