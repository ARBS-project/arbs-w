// SPDX-License-Identifier: GPL-3.0-or-later
#include "arbs_update.h"

#include <cstddef>

namespace arbsw {
namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    }
    return true;
}

}  // namespace

bool parseReleaseVersion(const std::string& text, std::array<int, 3>& out) {
    size_t i = (!text.empty() && text[0] == 'v') ? 1 : 0;
    std::array<int, 3> parsed{};
    for (size_t part = 0; part < parsed.size(); part++) {
        const size_t start = i;
        int value = 0;
        while (i < text.size() && isDigit(text[i])) {
            if (i - start >= 6) return false;  // keeps the value far from int overflow
            value = value * 10 + (text[i] - '0');
            i++;
        }
        if (i == start) return false;
        parsed[part] = value;
        if (part + 1 < parsed.size()) {
            if (i >= text.size() || text[i] != '.') return false;
            i++;
        }
    }
    if (i != text.size()) return false;
    out = parsed;
    return true;
}

std::string releaseVersionText(const std::string& tag) {
    std::array<int, 3> v{};
    if (!parseReleaseVersion(tag, v)) return std::string();
    return tag[0] == 'v' ? tag.substr(1) : tag;
}

bool isNewerRelease(const std::string& current, const std::string& tag) {
    std::array<int, 3> cur{}, latest{};
    if (!parseReleaseVersion(current, cur) || !parseReleaseVersion(tag, latest)) return false;
    return latest > cur;
}

bool isTrustedReleaseUrl(const std::string& url) {
    static const std::string host = "https://github.com/";
    static const std::string org = "arbs-project";
    static const std::string repo = "/arbs-w/releases/";
    if (url.compare(0, host.size(), host) != 0) return false;
    if (url.size() < host.size() + org.size() + repo.size()) return false;
    if (!equalsIgnoreCase(url.substr(host.size(), org.size()), org)) return false;
    if (url.compare(host.size() + org.size(), repo.size(), repo) != 0) return false;
    const std::string rest = url.substr(host.size() + org.size() + repo.size());
    if (rest.find("..") != std::string::npos) return false;
    for (char c : rest) {
        const bool plain = isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' || c == '_' || c == '/';
        if (!plain) return false;
    }
    return true;
}

std::string updateUserAgent(const std::string& version) {
    return "ARBS-W/" + version;
}

}  // namespace arbsw
