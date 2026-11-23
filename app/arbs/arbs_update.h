// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W R1b (D-R1b-6): rules for the optional update notice (GitHub Releases of arbs-project/arbs-w). No Qt, no I/O.
#pragma once

#include <array>
#include <string>

namespace arbsw {

inline constexpr const char* kLatestReleaseApiUrl = "https://api.github.com/repos/arbs-project/arbs-w/releases/latest";
inline constexpr const char* kReleasesPageUrl = "https://github.com/arbs-project/arbs-w/releases";

// "0.1.2" or "v0.1.2" (exactly three numeric parts). Anything else -> false.
bool parseReleaseVersion(const std::string& text, std::array<int, 3>& out);

// Tag without the leading "v"; empty when the tag is not a release version.
std::string releaseVersionText(const std::string& tag);

// True only when both parse and tag > current.
bool isNewerRelease(const std::string& current, const std::string& tag);

// https://github.com/<arbs-project, any case>/arbs-w/releases/<plain path>. No "..", query, fragment or credentials.
bool isTrustedReleaseUrl(const std::string& url);

// GitHub's API answers 403 without a User-Agent.
std::string updateUserAgent(const std::string& version);

}  // namespace arbsw
