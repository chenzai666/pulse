#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::app {

inline constexpr int kFolderSourceControl = 0x6F51;
inline constexpr wchar_t kFolderSourceMarker[] = L"Pulse.FolderSource.v1";

inline bool IsFolderSourcePath(std::wstring_view folder) {
    if (folder.empty() || folder.size() >= 32767 || folder.find_first_of(L"\r\n\"*?<>|") != folder.npos)
        return false;
    for (const wchar_t value : folder) if (value < 32) return false;
    const bool drive = folder.size() >= 3 &&
        ((folder[0] >= L'A' && folder[0] <= L'Z') || (folder[0] >= L'a' && folder[0] <= L'z')) &&
        folder[1] == L':' && folder[2] == L'\\';
    if (drive) return folder.find(L':', 2) == folder.npos;
    if (!folder.starts_with(L"\\\\") || folder.starts_with(L"\\\\.\\") ||
        folder.find(L':') != folder.npos) return false;
    const size_t server = folder.find(L'\\', 2);
    return server != folder.npos && server > 2 && server + 1 < folder.size() && folder[server + 1] != L'\\';
}

// A standard read-only child Edit lets local file-dialog integrations read the
// active folder through marshalled WM_GETTEXT, without a navigation endpoint.
inline void PublishFolderSource(HWND window, std::wstring_view folder, bool enabled) {
    HWND control = GetDlgItem(window, kFolderSourceControl);
    if (!enabled) {
        RemovePropW(window, kFolderSourceMarker);
        if (control && GetParent(control) == window) DestroyWindow(control);
        return;
    }
    if (!control) {
        control = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_READONLY,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFolderSourceControl)), GetModuleHandleW(nullptr), nullptr);
        if (!control) return;
    }
    if (GetParent(control) != window) return;
    if (!IsFolderSourcePath(folder)) folder = {};
    const int length = GetWindowTextLengthW(control);
    std::wstring previous(static_cast<size_t>(length) + 1, L'\0');
    previous.resize(static_cast<size_t>(GetWindowTextW(control, previous.data(), length + 1)));
    if (previous != folder) {
        const std::wstring text(folder);
        if (!SetWindowTextW(control, text.c_str())) return;
    }
    SetPropW(window, kFolderSourceMarker, reinterpret_cast<HANDLE>(1));
}

} // namespace pulse::app
