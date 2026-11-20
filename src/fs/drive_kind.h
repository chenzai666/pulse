#pragma once
#include <windows.h>
#include <string_view>

namespace pulse::fs {

inline bool HasDrivePathPrefix(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
        CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
                             static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

inline wchar_t DriveLetterOf(std::wstring_view path) {
    if (HasDrivePathPrefix(path, L"\\\\?\\") || HasDrivePathPrefix(path, L"\\\\.\\"))
        path.remove_prefix(4);
    if (path.size() < 2 || path[1] != L':') return 0;
    const wchar_t letter = path[0];
    if (letter >= L'a' && letter <= L'z') return static_cast<wchar_t>(letter - L'a' + L'A');
    return letter >= L'A' && letter <= L'Z' ? letter : 0;
}

inline bool IsHardDiskVolumeDevice(std::wstring_view target) {
    constexpr std::wstring_view prefix = L"\\Device\\HarddiskVolume";
    if (!HasDrivePathPrefix(target, prefix)) return false;
    const auto number = target.substr(prefix.size());
    if (number.empty()) return false;
    for (const wchar_t digit : number) {
        if (digit < L'0' || digit > L'9') return false;
    }
    return true;
}

} // namespace pulse::fs
