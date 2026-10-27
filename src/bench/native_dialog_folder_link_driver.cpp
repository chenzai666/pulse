#include "../app/native_dialog_folder_link.h"

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

// Test-only driver. It can inspect only the exact fixture child and private
// desktop specified by the parent; production builds omit the tested entry.
int wmain(int argc, wchar_t** argv) {
    if (argc != 6 || !std::wstring_view(argv[1]).starts_with(L"PulseFolderProbe_")) return 2;
    wchar_t* end = nullptr;
    const auto raw = _wcstoui64(argv[2], &end, 10);
    if (!raw || !end || *end) return 2;
    const HWND dialog = reinterpret_cast<HWND>(static_cast<uintptr_t>(raw));
    const auto expected_pid = wcstoul(argv[3], &end, 10);
    if (!expected_pid || !end || *end) return 2;
    const bool expect_accept = std::wstring_view(argv[5]) == L"accept";
    if (!expect_accept && std::wstring_view(argv[5]) != L"reject") return 2;
    constexpr DWORD access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW |
        DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL;
    HDESK desktop = OpenDesktopW(argv[1], 0, FALSE, access);
    if (!desktop) return 3;
    DWORD pid = 0;
    wchar_t name[64]{};
    const bool valid = GetWindowThreadProcessId(dialog, &pid) && pid == expected_pid &&
        GetClassNameW(dialog, name, ARRAYSIZE(name)) && std::wstring_view(name) == L"#32770";
    const bool result = valid && pulse::app::NativeDialogFolderLinkNavigateForTest(dialog, argv[4], desktop, true);
    CloseDesktop(desktop);
    std::printf("issued=%d expected=%d\n", result ? 1 : 0, expect_accept ? 1 : 0);
    return result == expect_accept ? 0 : 1;
}
