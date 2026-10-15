#pragma once
#include "../common/current_user_security.h"
#include <aclapi.h>
#include <cstdint>
#include <cwctype>
#include <string_view>

namespace pulse::index::custom_storage {

inline std::wstring InstallDirectory() {
    std::wstring module(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!count || count >= module.size()) return {};
    module.resize(count);
    const auto slash = module.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : module.substr(0, slash);
}

// Keep data beside this copy of the executable, regardless of CWD or AppData.
inline std::wstring RootPath() {
    const auto directory = InstallDirectory();
    return directory.empty() ? std::wstring{} : directory + L"\\index";
}

inline std::wstring InstallationKey(std::wstring_view directory = InstallDirectory()) {
    uint64_t hash = 1469598103934665603ull;
    for (const wchar_t ch : directory) {
        hash ^= static_cast<uint16_t>(towlower(ch == L'/' ? L'\\' : ch));
        hash *= 1099511628211ull;
    }
    return std::to_wstring(hash);
}

inline bool EnsurePrivateDirectory(const std::wstring& path) {
    const auto sid = CurrentUserSidString();
    if (path.empty() || sid.empty()) { SetLastError(ERROR_PATH_NOT_FOUND); return false; }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto sddl = L"O:" + sid + L"D:P(A;OICI;FA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const bool created = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    if (!created && create_error != ERROR_ALREADY_EXISTS) {
        LocalFree(descriptor); SetLastError(create_error); return false;
    }
    HANDLE directory = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE) { LocalFree(descriptor); return false; }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    bool ok = GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR actual = nullptr;
    PACL dacl = nullptr;
    if (ok) ok = GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &dacl, nullptr, &actual) == ERROR_SUCCESS &&
        owner && sid == [&] {
            LPWSTR value = nullptr;
            if (!ConvertSidToStringSidW(owner, &value)) return std::wstring{};
            const std::wstring result(value); LocalFree(value); return result;
        }();
    if (ok) ok = dacl != nullptr;
    for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(dacl, i, &raw)) { ok = false; break; }
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
        const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        LPWSTR value = nullptr;
        if (!ConvertSidToStringSidW(const_cast<DWORD*>(&ace->SidStart), &value)) { ok = false; break; }
        const std::wstring allowed(value); LocalFree(value);
        ok = allowed == sid || allowed == L"S-1-5-18" || allowed == L"S-1-5-32-544";
    }
    if (actual) LocalFree(actual);
    CloseHandle(directory);
    LocalFree(descriptor);
    if (!ok) SetLastError(ERROR_ACCESS_DENIED);
    return ok;
}

inline std::wstring Directory(std::wstring_view child = {}) {
    std::wstring root = RootPath();
    if (root.empty() || !EnsurePrivateDirectory(root)) return {};
    if (!child.empty()) {
        // Callers select a single internal subdirectory, never an arbitrary path.
        if (child == L"." || child == L".." || child.find_first_of(L"\\/:") != std::wstring_view::npos) {
            SetLastError(ERROR_INVALID_PARAMETER); return {};
        }
        root += L"\\"; root += child;
        if (!EnsurePrivateDirectory(root)) return {};
    }
    return root;
}

inline bool Writable() {
    const auto root = Directory();
    if (root.empty()) return false;
    const auto probe = root + L"\\.write-check-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CloseHandle(file); return true;
}
}
