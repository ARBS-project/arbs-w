// SPDX-License-Identifier: GPL-3.0-or-later
// ARBS-W log file (ILog). Same policy as ARBS-A ArbsLog.java: one file per run, "[+  s.mmm s] line", 40 MB hard cap,
// 1 s lines only while the file is <= 20 MB unless verbose (ArbsSessionStats.perSecondAllowed). Lines that report a
// state change can also be forwarded to a mirror sink (the Moonlight log). Never log IP addresses, tokens or PINs:
// callers only pass controller numbers, counters and device state.
// Pure C++ + C stdio (no Qt) so the policy is unit-tested; the bridge supplies the folder and the mirror.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>

#include "arbs/platform.h"

namespace arbsw {

class ArbsLogFile final : public arbs::ILog {
public:
    static constexpr int64_t kMaxFileBytes = 40LL * 1024 * 1024;

    // [path] = full file path (UTF-16). The file is created (truncated) immediately. [startNs] = time base for "+s".
    ArbsLogFile(const std::wstring& path, std::function<int64_t()> nowNs);
    ~ArbsLogFile() override;
    ArbsLogFile(const ArbsLogFile&) = delete;
    ArbsLogFile& operator=(const ArbsLogFile&) = delete;

    void info(const std::string& utf8) override;
    bool perSecondAllowed() override;
    void setVerbose(bool v) override { verbose_ = v; }

    // Lines for which [filter] returns true are also passed to [sink] (e.g. SDL_LogInfo into the Moonlight log).
    void setMirror(std::function<bool(const std::string&)> filter, std::function<void(const std::string&)> sink);
    // Crash path: info() only try-locks for a short time and never blocks the faulting thread forever.
    void setCrashMode() { crashMode_ = true; }

    bool ok() const { return f_ != nullptr; }
    int64_t bytesWritten() const { return written_.load(); }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
    std::function<int64_t()> nowNs_;
    int64_t startNs_ = 0;
    std::FILE* f_ = nullptr;
    std::timed_mutex mu_;
    std::atomic<int64_t> written_{0};
    std::atomic<bool> verbose_{false};
    std::atomic<bool> crashMode_{false};
    std::function<bool(const std::string&)> mirrorFilter_;
    std::function<void(const std::string&)> mirrorSink_;
};

}  // namespace arbsw
