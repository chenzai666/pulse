#pragma once

#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::app {

// A standard read-only child Edit lets local file-dialog integrations read the
// active folder through marshalled WM_GETTEXT, without a navigation endpoint.
inline void PublishFolderSource(HWND window, std::wstring_view folder, bool enabled) {
    constexpr int control_id = 0x6F51;
    constexpr wchar_t marker[] = L"Pulse.FolderSource.v1";
    HWND control = GetDlgItem(window, control_id);
    if (!enabled) {
        RemovePropW(window, marker);
        if (control && GetParent(control) == window) DestroyWindow(control);
        return;
    }
    if (!control) {
        control = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_READONLY,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), GetModuleHandleW(nullptr), nullptr);
        if (!control) return;
    }
    if (GetParent(control) != window) return;
    const int length = GetWindowTextLengthW(control);
    std::wstring previous(static_cast<size_t>(length) + 1, L'\0');
    previous.resize(static_cast<size_t>(GetWindowTextW(control, previous.data(), length + 1)));
    if (previous != folder) {
        const std::wstring text(folder);
        if (!SetWindowTextW(control, text.c_str())) return;
    }
    SetPropW(window, marker, reinterpret_cast<HANDLE>(1));
}

} // namespace pulse::app
