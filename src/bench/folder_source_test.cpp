#include "../app/folder_source.h"
#include <cstdio>
#include <string>

int wmain() {
    HWND window = CreateWindowExW(0, L"STATIC", L"folder source test", WS_OVERLAPPED,
                                  0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 1;
    auto check = [&](std::wstring_view folder, const wchar_t* expected) {
        pulse::app::PublishFolderSource(window, folder, true);
        HWND edit = GetDlgItem(window, pulse::app::kFolderSourceControl);
        wchar_t text[256]{};
        if (edit) GetWindowTextW(edit, text, 256);
        return edit && GetParent(edit) == window &&
               GetPropW(window, pulse::app::kFolderSourceMarker) == reinterpret_cast<HANDLE>(1) &&
               std::wstring_view(text) == expected;
    };
    const bool valid = check(L"C:\\test\\first", L"C:\\test\\first") &&
                       check(L"C:\\test\\second", L"C:\\test\\second") &&
                       check(L"pulse:home", L"") &&
                       check(L"\\\\server\\share\\folder", L"\\\\server\\share\\folder");
    pulse::app::PublishFolderSource(window, {}, false);
    const bool removed = !GetDlgItem(window, pulse::app::kFolderSourceControl) &&
                         !GetPropW(window, pulse::app::kFolderSourceMarker);
    DestroyWindow(window);
    std::puts(valid && removed ? "[PASS] folder source lifecycle" : "[FAIL] folder source lifecycle");
    return valid && removed ? 0 : 1;
}
