#pragma once
#include "content_search_protocol.h"
#include "../common/current_user_security.h"
#include "custom_index_storage.h"
#include <algorithm>
#include <cwctype>

namespace pulse::index::config_storage {
inline bool SameStoredPath(const std::wstring& a, const std::wstring& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }
inline std::wstring DatabasePath() {
#ifdef PULSE_CUSTOM_EDITION
    const auto dir = custom_storage::Directory(L"ContentIndex");
    return dir.empty() ? std::wstring{} : dir + L"\\content-v1.sqlite";
#else
    wchar_t base[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base));
    if (!n || n >= ARRAYSIZE(base)) return {};
    std::wstring dir = std::wstring(base) + L"\\Pulse";
    CurrentUserSecurityAttributes security;
    CreateDirectoryW(dir.c_str(), security ? security.get() : nullptr);
    dir += L"\\ContentIndex";
    CreateDirectoryW(dir.c_str(), security ? security.get() : nullptr);
    return dir + L"\\content-v1.sqlite";
#endif
}
inline std::wstring ObjectName(const std::wstring& path, const wchar_t* kind) {
    wchar_t absolute[32768]{};
    const DWORD n = GetFullPathNameW(path.c_str(), ARRAYSIZE(absolute), absolute, nullptr);
    const std::wstring_view value = n && n < ARRAYSIZE(absolute) ? std::wstring_view(absolute, n) : std::wstring_view(path);
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t ch : value) { hash ^= static_cast<uint16_t>(towlower(ch)); hash *= 1099511628211ull; }
    return L"Local\\PulseContent." + std::wstring(kind) + L"." + std::to_wstring(hash);
}
inline HANDLE NamedMutex(const std::wstring& path, const wchar_t* kind) {
    CurrentUserSecurityAttributes security;
    return security ? CreateMutexW(security.get(), FALSE, ObjectName(path, kind).c_str()) : nullptr;
}
inline HANDLE NamedEvent(const std::wstring& path, const wchar_t* kind, bool manual) {
    CurrentUserSecurityAttributes security;
    return security ? CreateEventW(security.get(), manual, FALSE, ObjectName(path, kind).c_str()) : nullptr;
}
inline bool SameConfig(const ContentIndexConfig& a, const ContentIndexConfig& b) {
    if (a.shared_scope != b.shared_scope || a.excluded_paths != b.excluded_paths || a.default_encoding != b.default_encoding ||
        a.maximum_document_bytes != b.maximum_document_bytes) return false;
    if (a.maximum_file_bytes != b.maximum_file_bytes || a.roots.size() != b.roots.size() || a.excluded_directories.size() != b.excluded_directories.size()) return false;
    for (const auto& root : a.roots) if (std::none_of(b.roots.begin(), b.roots.end(), [&](const auto& other) { return SameStoredPath(root.path, other.path) && root.encoding == other.encoding; })) return false;
    for (const auto& ex : a.excluded_directories) if (std::none_of(b.excluded_directories.begin(), b.excluded_directories.end(), [&](const auto& other) { return SameStoredPath(ex, other); })) return false;
    return true;
}
inline bool ReadSidecar(const std::wstring& path, uint32_t type, std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 8 && size.QuadPart <= static_cast<LONGLONG>(content::kMaximumPayload);
    if (ok) { bytes.resize(static_cast<size_t>(size.QuadPart)); DWORD n = 0; ok = ::ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &n, nullptr) && n == bytes.size(); }
    CloseHandle(h);
    if (!ok) return false;
    ipc::PayloadReader r(bytes.data(), bytes.size()); uint32_t magic = 0, actual = 0;
    return r.GetU32(magic) && r.GetU32(actual) && magic == 0x58494350 && actual == type;
}
inline bool WriteSidecar(const std::wstring& path, uint32_t type, const std::vector<uint8_t>& payload) {
    const auto temp = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint32_t header[]{0x58494350, type}; DWORD n = 0;
    bool ok = WriteFile(h, header, sizeof(header), &n, nullptr) && n == sizeof(header) &&
        WriteFile(h, payload.data(), static_cast<DWORD>(payload.size()), &n, nullptr) && n == payload.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}
inline bool ReadConfigFile(const std::wstring& path, ContentIndexConfig& config) {
    std::vector<uint8_t> bytes;
    if (!ReadSidecar(path + L".config", 1, bytes)) return false;
    ipc::PayloadReader r(bytes.data() + 8, bytes.size() - 8);
    return content::GetConfig(r, config) && r.remaining() == 0;
}
inline bool WriteConfigFile(const std::wstring& path, const ContentIndexConfig& config) {
    HANDLE mutex = NamedMutex(path, L"config");
    if (!mutex) return false;
    const DWORD wait = WaitForSingleObject(mutex, 1000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return false; }
    ContentIndexConfig existing;
    bool ok = ReadConfigFile(path, existing) && SameConfig(existing, config);
    if (!ok) { ipc::PayloadWriter w; content::PutConfig(w, config); ok = WriteSidecar(path + L".config", 1, w.data()); }
    ReleaseMutex(mutex); CloseHandle(mutex); return ok;
}
inline bool LoadOrCreateConfig(const std::wstring& path, const ContentIndexConfig& fallback, ContentIndexConfig& actual) {
    HANDLE mutex = NamedMutex(path, L"config");
    if (!mutex) return false;
    const DWORD wait = WaitForSingleObject(mutex, 5000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return false; }
    bool ok = ReadConfigFile(path, actual);
    if (!ok) { actual = fallback; ipc::PayloadWriter w; content::PutConfig(w, actual); ok = WriteSidecar(path + L".config", 1, w.data()); }
    ReleaseMutex(mutex); CloseHandle(mutex); return ok;
}
inline bool ReadStatusFile(const std::wstring& path, ContentIndexStatus& status) {
    std::vector<uint8_t> bytes;
    if (!ReadSidecar(path + L".status", 2, bytes)) return false;
    ipc::PayloadReader r(bytes.data() + 8, bytes.size() - 8);
    return content::GetStatus(r, status) && r.remaining() == 0;
}
}
