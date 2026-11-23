// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W R1b (D-R1b-3, D-R1b-5): settings identity separate from official Moonlight, and the one-time portable
// settings copy from the old location. No Qt here so that the unit tests can run it on real files.
#pragma once

#include <filesystem>
#include <string>

namespace arbsw {

inline constexpr const char* kSettingsOrgName = "ARBS";
inline constexpr const char* kSettingsAppName = "ARBS-W";
inline constexpr const char* kLegacySettingsOrgName = "Moonlight Game Streaming Project";
inline constexpr const char* kLegacySettingsAppName = "Moonlight";

enum class SettingsIdentityAction {
    UseNew,            // new INI exists, or there is nothing to move
    Migrated,          // old INI copied to the new location (the old file is kept)
    LegacyReadOnly,    // read-only mode with only the old INI: read it in place, write nothing
    LegacyAfterError,  // copy failed: run on the old identity this time and retry next start
};

struct SettingsIdentityResult {
    SettingsIdentityAction action = SettingsIdentityAction::UseNew;
    std::string note;  // one log line without any path; empty when there is nothing to report

    bool useLegacyIdentity() const {
        return action == SettingsIdentityAction::LegacyReadOnly || action == SettingsIdentityAction::LegacyAfterError;
    }
};

// portableDir = folder holding portable.dat. Never overwrites an existing new INI and never changes the old INI.
// readOnly = true (--arbs-dump-prefs) creates nothing.
SettingsIdentityResult prepareSettingsIdentity(const std::filesystem::path& portableDir, bool readOnly);

}  // namespace arbsw
