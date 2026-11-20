// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W log file. See arbs_log.h.
#include "arbs_log.h"

#include <chrono>
#ifdef _WIN32
#include <share.h>
#endif

#include "arbs/java_compat.h"
#include "arbs/session_stats.h"

namespace arbsw {

ArbsLogFile::ArbsLogFile(const std::wstring& path, std::function<int64_t()> nowNs) : path_(path), nowNs_(std::move(nowNs)) {
    startNs_ = nowNs_ ? nowNs_() : 0;
#ifdef _WIN32
    // _SH_DENYNO: the log can be opened in Notepad while ARBS-W is running (fopen_s would deny other writers/readers)
    f_ = _wfsopen(path_.c_str(), L"wb", _SH_DENYNO);
#else
    f_ = nullptr;  // ARBS-W is Windows only
#endif
}

ArbsLogFile::~ArbsLogFile() {
    std::lock_guard<std::timed_mutex> g(mu_);
    if (f_ != nullptr) {
        std::fclose(f_);
        f_ = nullptr;
    }
}

void ArbsLogFile::setMirror(std::function<bool(const std::string&)> filter, std::function<void(const std::string&)> sink) {
    std::lock_guard<std::timed_mutex> g(mu_);
    mirrorFilter_ = std::move(filter);
    mirrorSink_ = std::move(sink);
}

void ArbsLogFile::info(const std::string& utf8) {
    std::unique_lock<std::timed_mutex> g(mu_, std::defer_lock);
    if (crashMode_) {
        if (!g.try_lock_for(std::chrono::milliseconds(200))) return;
    } else {
        g.lock();
    }
    const int64_t now = nowNs_ ? nowNs_() : 0;
    const std::string line = arbs::jc::fmt("[+%7.3fs] ", (now - startNs_) / 1e9) + utf8 + "\n";
    if (f_ != nullptr && written_.load() <= kMaxFileBytes) {
        const size_t n = std::fwrite(line.data(), 1, line.size(), f_);
        std::fflush(f_);
        written_ += static_cast<int64_t>(n);
    }
    if (!crashMode_ && mirrorSink_ && mirrorFilter_ && mirrorFilter_(utf8)) {
        try {
            mirrorSink_(utf8);
        } catch (...) {
        }
    }
}

bool ArbsLogFile::perSecondAllowed() { return arbs::SessionStats::perSecondAllowed(written_.load(), verbose_.load()); }

}  // namespace arbsw
