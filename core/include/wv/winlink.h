// Windows-only helpers shared by the APO, the app and wvinstall:
//   - the data folder %ProgramData%\WarmVoice (log, report, fallback link file)
//   - opening the shared block: "Global\WarmVoice_<guid>" first; if the audio engine is not
//     allowed to create global objects on this PC, a memory-mapped file in the data folder
//     (FILE_ATTRIBUTE_TEMPORARY keeps it in memory)
//   - a small append-only log the APO writes, so we can see what Windows did with it
// SPDX-License-Identifier: MIT
#pragma once
#ifndef _WIN32
#error "winlink.h is Windows only"
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <cstdarg>
#include <cstring>
#include <cwctype>
#include <cstdio>
#include <string>

#include "wv/shared.h"

namespace wv {

inline std::wstring dataDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = ExpandEnvironmentStringsW(L"%ProgramData%", buf, MAX_PATH);
    std::wstring d = (n > 0 && n < MAX_PATH && buf[0] != L'%') ? std::wstring(buf) : std::wstring(L"C:\\ProgramData");
    return d + L"\\WarmVoice";
}

inline std::wstring linkName(const std::wstring& guid) { return L"Global\\WarmVoice_" + guid; }
inline std::wstring linkFile(const std::wstring& guid) {
    std::wstring g;
    for (wchar_t c : guid) if (c != L'{' && c != L'}') g += c;
    return dataDir() + L"\\link-" + g + L".bin";
}

// Endpoint id "{0.0.1.00000000}.{guid}" -> "{guid}" in lower case (object names are case sensitive).
inline std::wstring guidFromEndpointId(const std::wstring& id) {
    const size_t dot = id.find_last_of(L'.');
    std::wstring g = dot == std::wstring::npos ? id : id.substr(dot + 1);
    for (auto& c : g) c = (wchar_t)towlower(c);
    return g;
}

// Append one line to %ProgramData%\WarmVoice\<file>. Never throws; silently gives up.
inline void logTo(const wchar_t* file, const char* fmt, ...) {
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour,
                          t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    if (n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int m = std::vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    n = (int)strnlen(line, sizeof line - 2);
    line[n++] = '\r';
    line[n++] = '\n';
    const std::wstring path = dataDir() + L"\\" + file;
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 512 * 1024) {   // keep it small: start over
        CloseHandle(h);
        h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
    }
    DWORD w = 0;
    WriteFile(h, line, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

struct LinkHandle {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE map = nullptr;
    SharedBlock* block = nullptr;
    bool viaFile = false;
    bool created = false;   // true when this call made a new block (not opened an existing one)

    void close() {
        if (block) UnmapViewOfFile((void*)block);
        if (map) CloseHandle(map);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        block = nullptr;
        map = nullptr;
        file = INVALID_HANDLE_VALUE;
        viaFile = created = false;
    }
};

// SYSTEM, LOCAL SERVICE (audiodg) and administrators: full; users: read/write.
inline PSECURITY_DESCRIPTOR linkSecurity() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;BU)(A;;GRGW;;;IU)",
                                                         SDDL_REVISION_1, &sd, nullptr);
    return sd;
}

// APO side: create (or open) the block. err1/err2 receive the Win32 errors of the two ways.
inline bool createLink(const std::wstring& guid, LinkHandle& L, DWORD* errGlobal = nullptr, DWORD* errFile = nullptr) {
    L.close();
    PSECURITY_DESCRIPTOR sd = linkSecurity();
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    L.map = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(SharedBlock),
                               linkName(guid).c_str());
    DWORD e = GetLastError();
    if (L.map) {
        L.created = e != ERROR_ALREADY_EXISTS;
        if (errGlobal) *errGlobal = 0;
    } else {
        if (errGlobal) *errGlobal = e;
        // Fallback: a temporary file in the data folder (the installer lets users and the audio
        // engine read/write there).
        L.file = CreateFileW(linkFile(guid).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             sd ? &sa : nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
        if (L.file != INVALID_HANDLE_VALUE) {
            L.created = GetLastError() != ERROR_ALREADY_EXISTS;
            L.map = CreateFileMappingW(L.file, nullptr, PAGE_READWRITE, 0, (DWORD)sizeof(SharedBlock), nullptr);
        }
        if (errFile) *errFile = L.map ? 0 : GetLastError();
        L.viaFile = true;
    }
    if (sd) LocalFree(sd);
    if (L.map) L.block = (SharedBlock*)MapViewOfFile(L.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock));
    if (!L.block) { L.close(); return false; }
    return true;
}

// App / installer side: open an existing block (never creates one).
inline bool openLink(const std::wstring& guid, LinkHandle& L, bool write) {
    L.close();
    const DWORD acc = write ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;
    L.map = OpenFileMappingW(acc, FALSE, linkName(guid).c_str());
    if (!L.map) {
        L.file = CreateFileW(linkFile(guid).c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0),
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (L.file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(L.file, &sz) || sz.QuadPart < (LONGLONG)sizeof(SharedBlock)) { L.close(); return false; }
        L.map = CreateFileMappingW(L.file, nullptr, write ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
        L.viaFile = true;
    }
    if (L.map) L.block = (SharedBlock*)MapViewOfFile(L.map, acc, 0, 0, sizeof(SharedBlock));
    if (!L.block || L.block->magic != kSharedMagic || L.block->size != sizeof(SharedBlock)) { L.close(); return false; }
    return true;
}

}  // namespace wv
