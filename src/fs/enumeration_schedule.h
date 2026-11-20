#pragma once
#include "drive_kind.h"
#include <iterator>

namespace pulse::fs {

// Scheduling uses DOS device mappings only. Filesystem or volume queries
// could block the UI thread that submits a navigation request.
template<class Query>
bool NeedsBoundedEnumeration(std::wstring_view path, Query query) {
    const bool extended = HasDrivePathPrefix(path, L"\\\\?\\") || HasDrivePathPrefix(path, L"\\\\.\\");
    if (extended) path.remove_prefix(4);
    else if (HasDrivePathPrefix(path, L"\\\\")) return true;
    if (HasDrivePathPrefix(path, L"UNC\\")) return true;
    wchar_t letter = DriveLetterOf(path);
    if (!letter) return extended;
    unsigned visited = 0;
    for (unsigned depth = 0; depth < 8; ++depth) {
        const unsigned bit = 1u << (letter - L'A');
        if (visited & bit) return true;
        visited |= bit;
        const wchar_t device[] = {letter, L':', L'\0'};
        wchar_t buffer[1024]{};
        const DWORD length = query(device, buffer, static_cast<DWORD>(std::size(buffer)));
        if (!length || length >= std::size(buffer)) return true;
        const std::wstring_view mappings(buffer, length);
        const auto target = mappings.substr(0, mappings.find(L'\0'));
        if (IsHardDiskVolumeDevice(target)) return false;
        if (!HasDrivePathPrefix(target, L"\\??\\")) return true;
        letter = DriveLetterOf(target.substr(4));
        if (!letter) return true;
    }
    return true;
}

static inline bool NeedsBoundedEnumeration(std::wstring_view path) {
    return NeedsBoundedEnumeration(path, QueryDosDeviceW);
}

} // namespace pulse::fs
