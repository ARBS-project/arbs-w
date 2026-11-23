// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "arbs_identity.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace arbsw {
namespace {

enum class Presence { Present, Missing, Unknown };

Presence presenceOf(const fs::path& p) {
    std::error_code ec;
    const fs::file_status s = fs::status(p, ec);
    if (s.type() == fs::file_type::not_found) return Presence::Missing;
    if (ec || s.type() == fs::file_type::none || s.type() == fs::file_type::unknown) return Presence::Unknown;
    return Presence::Present;
}

bool readAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

bool flushToDisk(const fs::path& p) {
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    return ok != FALSE;
}

// Temporary file -> byte comparison -> rename that refuses to replace an existing file.
bool copyVerified(const fs::path& from, const fs::path& tmp, const fs::path& to, std::string& why) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (ec) {
        why = "폴더 만들기 실패 " + std::to_string(ec.value());
        return false;
    }
    if (!fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec) || ec) {
        why = "복사 실패 " + std::to_string(ec.value());
        return false;
    }
    std::string a, b;
    if (!readAll(from, a) || !readAll(tmp, b) || a != b) {
        why = "복사본 내용 불일치";
        return false;
    }
    if (!flushToDisk(tmp)) {
        why = "디스크 기록 실패 " + std::to_string(GetLastError());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        why = "이름 바꾸기 실패 " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

}  // namespace

SettingsIdentityResult prepareSettingsIdentity(const fs::path& portableDir, bool readOnly) {
    const fs::path oldIni = portableDir / L"Moonlight Game Streaming Project" / L"Moonlight.ini";
    const fs::path newDir = portableDir / L"ARBS";
    const fs::path newIni = newDir / L"ARBS-W.ini";
    const fs::path tmpIni = newDir / L"ARBS-W.ini.migrating";

    SettingsIdentityResult result;
    const Presence newState = presenceOf(newIni);
    if (newState == Presence::Present) return result;
    const Presence oldState = presenceOf(oldIni);
    if (oldState == Presence::Missing) return result;

    if (readOnly) {
        result.action = SettingsIdentityAction::LegacyReadOnly;
        return result;
    }
    if (oldState == Presence::Unknown || newState == Presence::Unknown) {
        result.action = SettingsIdentityAction::LegacyAfterError;
        result.note = "ARBS-W 경고: 설정 위치를 확인하지 못해 이번 실행은 이전 설정 위치를 사용(다음 실행 때 다시 시도)";
        return result;
    }

    std::string why;
    if (copyVerified(oldIni, tmpIni, newIni, why)) {
        result.action = SettingsIdentityAction::Migrated;
        result.note = "ARBS-W: 이전 설정·페어링을 새 설정 위치로 복사함(원본 유지)";
        return result;
    }

    std::error_code ignored;
    fs::remove(tmpIni, ignored);
    if (presenceOf(newIni) == Presence::Present) return result;  // created by another instance meanwhile
    result.action = SettingsIdentityAction::LegacyAfterError;
    result.note = "ARBS-W 경고: 설정 이전 실패(" + why + ") - 이번 실행은 이전 설정 위치를 사용(다음 실행 때 다시 시도)";
    return result;
}

}  // namespace arbsw
