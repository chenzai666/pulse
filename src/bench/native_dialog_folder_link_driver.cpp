#include "../app/native_dialog_folder_link.h"

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {
struct ProbeControls {
    int shell_views = 0;
    int bands = 0;
    int toolbars = 0;
    HWND focus_candidate = nullptr;
};
BOOL CALLBACK CountControls(HWND child, LPARAM value) {
    auto& controls = *reinterpret_cast<ProbeControls*>(value);
    wchar_t name[96]{};
    if (!GetClassNameW(child, name, ARRAYSIZE(name)) || !IsWindowVisible(child)) return TRUE;
    const std::wstring_view cls(name);
    const int id = GetDlgCtrlID(child);
    if (cls == L"DUIViewWndClassName") ++controls.shell_views;
    if (cls == L"Address Band Root" && id == 41477) ++controls.bands;
    if (cls == L"ToolbarWindow32" && id == 1001) ++controls.toolbars;
    if (cls == L"Edit" && (id == 1001 || id == 1148 || id == 1152) && !controls.focus_candidate)
        controls.focus_candidate = child;
    return TRUE;
}
}

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
    if (!SetThreadDesktop(desktop)) { CloseDesktop(desktop); return 3; }
    DWORD pid = 0;
    wchar_t name[64]{};
    const DWORD target_thread = GetWindowThreadProcessId(dialog, &pid);
    const bool valid = target_thread && pid == expected_pid &&
        GetClassNameW(dialog, name, ARRAYSIZE(name)) && std::wstring_view(name) == L"#32770";
    ProbeControls controls;
    if (valid) EnumChildWindows(dialog, CountControls, reinterpret_cast<LPARAM>(&controls));
    GUITHREADINFO info{sizeof(info)};
    const bool before = valid && GetGUIThreadInfo(target_thread, &info);
    std::printf("valid=%d shell_views=%d bands=%d toolbars=%d gui=%d focus=%p capture=%p flags=%lu candidate=%p\n",
        valid ? 1 : 0, controls.shell_views, controls.bands, controls.toolbars, before ? 1 : 0,
        static_cast<void*>(info.hwndFocus), static_cast<void*>(info.hwndCapture), info.flags,
        static_cast<void*>(controls.focus_candidate));
    // A private desktop is never switched onto the user's display. Its test
    // dialog may have no active/focused control; put focus on its own edit as
    // a real user returning to the window would, without changing product checks.
    if (valid && before && !info.hwndFocus && controls.focus_candidate &&
        AttachThreadInput(GetCurrentThreadId(), target_thread, TRUE)) {
        SetActiveWindow(dialog);
        SetFocus(controls.focus_candidate);
        AttachThreadInput(GetCurrentThreadId(), target_thread, FALSE);
        info = {sizeof(info)};
        const bool after = GetGUIThreadInfo(target_thread, &info) != FALSE;
        std::printf("setup_gui=%d focus=%p capture=%p flags=%lu\n", after ? 1 : 0,
            static_cast<void*>(info.hwndFocus), static_cast<void*>(info.hwndCapture), info.flags);
    }
    const bool result = valid && pulse::app::NativeDialogFolderLinkNavigateForTest(dialog, argv[4], desktop, true);
    CloseDesktop(desktop);
    std::printf("issued=%d expected=%d\n", result ? 1 : 0, expect_accept ? 1 : 0);
    return result == expect_accept ? 0 : 1;
}
