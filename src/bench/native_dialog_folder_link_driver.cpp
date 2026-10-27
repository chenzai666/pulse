#include "../app/native_dialog_folder_link.h"

#include <windows.h>
#include <shellapi.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

bool RunFolderLinkSequence(HWND dialog, HDESK desktop, const std::wstring& first,
                           const std::wstring& second, const std::wstring& status, HWND initial_focus);
bool RunFolderLinkFault(HWND dialog, HDESK desktop, const std::wstring& folder,
                        const std::wstring& mapping_name, const std::wstring& marker, bool recover);

namespace {
struct ProbeControls {
    int shell_views = 0;
    int bands = 0;
    int toolbars = 0;
    HWND focus_candidate = nullptr;
    HWND band = nullptr;
    HWND toolbar = nullptr;
    HWND address = nullptr;
};
BOOL CALLBACK CountControls(HWND child, LPARAM value) {
    auto& controls = *reinterpret_cast<ProbeControls*>(value);
    wchar_t name[96]{};
    if (!GetClassNameW(child, name, ARRAYSIZE(name)) || !IsWindowVisible(child)) return TRUE;
    const std::wstring_view cls(name);
    const int id = GetDlgCtrlID(child);
    if (cls == L"DUIViewWndClassName") ++controls.shell_views;
    if (cls == L"Address Band Root" && id == 41477) { ++controls.bands; controls.band = child; }
    if (cls == L"ToolbarWindow32" && id == 1001) { ++controls.toolbars; controls.toolbar = child; }
    if (cls == L"Edit" && id == 41477) controls.address = child;
    if (cls == L"Edit" && (id == 1001 || id == 1148 || id == 1152) && !controls.focus_candidate)
        controls.focus_candidate = child;
    return TRUE;
}
bool Send(HWND window, UINT message, WPARAM wp, LPARAM lp, DWORD_PTR& result) {
    return SendMessageTimeoutW(window, message, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &result) != 0;
}
bool Read(HWND window, std::wstring& text) {
    wchar_t buffer[32768]{}; DWORD_PTR length = 0;
    if (!Send(window, WM_GETTEXT, ARRAYSIZE(buffer), reinterpret_cast<LPARAM>(buffer), length) || length >= ARRAYSIZE(buffer)) return false;
    text.assign(buffer, static_cast<size_t>(length)); return true;
}
bool RunRecovery(HWND dialog, DWORD thread, HDESK desktop, const std::wstring& target) {
    using Microsoft::WRL::ComPtr;
    ProbeControls controls; EnumChildWindows(dialog, CountControls, reinterpret_cast<LPARAM>(&controls));
    if (controls.bands != 1 || controls.toolbars != 1 || !IsChild(controls.band, controls.toolbar)) return false;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return false;
    ComPtr<IUIAutomation> automation; ComPtr<IUIAutomationElement> element; ComPtr<IUIAutomationTreeWalker> walker;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) &&
        SUCCEEDED(automation->ElementFromHandle(controls.toolbar, &element)) &&
        SUCCEEDED(automation->get_RawViewWalker(&walker));
    RECT bounds{}; ComPtr<IUIAutomationElement> child;
    if (ok) ok = SUCCEEDED(element->get_CurrentBoundingRectangle(&bounds)) && SUCCEEDED(walker->GetFirstChildElement(element.Get(), &child));
    LONG right = bounds.left; int count = 0;
    while (ok && child && count++ < 256) {
        RECT item{}; BOOL offscreen = TRUE;
        ok = SUCCEEDED(child->get_CurrentBoundingRectangle(&item)) && SUCCEEDED(child->get_CurrentIsOffscreen(&offscreen));
        if (ok && !offscreen && item.right > item.left && item.bottom > item.top &&
            item.right > bounds.left && item.left < bounds.right && item.bottom > bounds.top && item.top < bounds.bottom)
            right = (std::max)(right, (std::min)(bounds.right, item.right));
        ComPtr<IUIAutomationElement> next;
        if (ok) ok = SUCCEEDED(walker->GetNextSiblingElement(child.Get(), &next));
        child = std::move(next);
    }
    ok = ok && count > 0 && count <= 256 && !child && bounds.right - right >= 30;
    POINT point{right + (bounds.right - right) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
    RECT client{}; DWORD_PTR result = 0;
    if (ok) ok = ScreenToClient(controls.toolbar, &point) && GetClientRect(controls.toolbar, &client) &&
        point.x >= 0 && point.y >= 0 && point.x < client.right && point.y < client.bottom;
    if (ok) ok = Send(controls.toolbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y), result) &&
        Send(controls.toolbar, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y), result);
    controls.address = nullptr; EnumChildWindows(dialog, CountControls, reinterpret_cast<LPARAM>(&controls));
    GUITHREADINFO info{sizeof(info)};
    if (ok) ok = controls.address && IsChild(controls.band, controls.address) &&
        GetGUIThreadInfo(thread, &info) && info.hwndFocus == controls.address;
    std::wstring original, actual;
    if (ok) ok = Read(controls.address, original) && !original.empty();
    for (LONG stage : {3L, 4L, 5L, 6L}) {
        if (!ok) break;
        if (!Send(controls.address, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(target.c_str()), result) || !result) { ok = false; break; }
        const bool invoked = pulse::app::NativeDialogFolderLinkRecoverForTest(dialog, controls.band, controls.toolbar,
            controls.address, stage, original, target, desktop);
        const bool read = Read(controls.address, actual);
        const bool restored = read && actual == (stage >= 5 ? target : original);
        std::printf("recovery_stage=%ld invoked=%d read=%d expected_text=%d\n", stage, invoked ? 1 : 0, read ? 1 : 0, restored ? 1 : 0);
        ok = invoked && restored;
        // A user may edit the address after the helper stops. The recovery
        // record contains the old attempted text and must never overwrite it.
        const std::wstring user_edit = L"fixture-user-edited-address";
        if (ok) ok = Send(controls.address, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(user_edit.c_str()), result) && result;
        const bool user_invoked = ok && pulse::app::NativeDialogFolderLinkRecoverForTest(dialog, controls.band, controls.toolbar,
            controls.address, stage, original, target, desktop);
        const bool user_preserved = user_invoked && Read(controls.address, actual) && actual == user_edit;
        std::printf("recovery_stage=%ld user_input_preserved=%d\n", stage, user_preserved ? 1 : 0);
        ok = ok && user_preserved;
    }
    // Restore only this fixture's text; never commit it.
    if (controls.address && Read(controls.address, actual) &&
        (actual == target || actual == L"fixture-user-edited-address"))
        Send(controls.address, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(original.c_str()), result);
    child.Reset(); walker.Reset(); element.Reset(); automation.Reset(); CoUninitialize();
    return ok;
}
}

// Test-only driver. It can inspect only the exact fixture child and private
// desktop specified by the parent; production builds omit the tested entry.
int wmain(int argc, wchar_t** argv) {
    if ((argc != 6 && argc != 7 && argc != 8) || !std::wstring_view(argv[1]).starts_with(L"PulseFolderProbe_")) return 2;
    wchar_t* end = nullptr;
    const auto raw = _wcstoui64(argv[2], &end, 10);
    if (!raw || !end || *end) return 2;
    const HWND dialog = reinterpret_cast<HWND>(static_cast<uintptr_t>(raw));
    const auto expected_pid = wcstoul(argv[3], &end, 10);
    if (!expected_pid || !end || *end) return 2;
    const bool stale = std::wstring_view(argv[5]) == L"stale";
    const bool fault = std::wstring_view(argv[5]) == L"fault";
    const bool fault_recover = std::wstring_view(argv[5]) == L"fault-recover";
    const bool recover = std::wstring_view(argv[5]) == L"recover";
    const bool twice = std::wstring_view(argv[5]) == L"twice";
    const bool expect_accept = std::wstring_view(argv[5]) == L"accept";
    if (!fault && !fault_recover && !stale && !recover && !twice && !expect_accept && std::wstring_view(argv[5]) != L"reject") return 2;
    if ((twice && argc != 7) || ((fault || fault_recover) && argc != 8) ||
        (!twice && !fault && !fault_recover && argc != 6)) return 2;
    constexpr DWORD access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW |
        DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL;
    HDESK desktop = OpenDesktopW(argv[1], 0, FALSE, access);
    if (!desktop) return 3;
    if (!SetThreadDesktop(desktop)) { CloseDesktop(desktop); return 3; }
    if (stale) {
        // The parent closed its owned fixture and waited for its process to
        // exit before this invocation. Refuse a recycled HWND outright.
        const bool destroyed = !IsWindow(dialog);
        const bool invoked = destroyed && pulse::app::NativeDialogFolderLinkRecoverForTest(
            dialog, nullptr, nullptr, nullptr, 5, L"fixture-original", L"fixture-attempted", desktop);
        std::printf("destroyed=%d stale_recovery_rejected=%d\n", destroyed ? 1 : 0, !invoked ? 1 : 0);
        CloseDesktop(desktop);
        return destroyed && !invoked ? 0 : 1;
    }
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
    // A previous navigation may leave focus in the address band even after
    // its directory has changed. Each accept/recovery invocation represents
    // the user returning to this owned dialog: focus its known filename edit.
    // No product identity, focus, capture, or security check is relaxed.
    const bool needs_focus = expect_accept || twice || recover || fault;
    DWORD candidate_pid = 0;
    const bool candidate_valid = valid && controls.focus_candidate &&
        IsChild(dialog, controls.focus_candidate) &&
        GetWindowThreadProcessId(controls.focus_candidate, &candidate_pid) == target_thread &&
        candidate_pid == expected_pid;
    if (needs_focus && before && candidate_valid && info.hwndFocus != controls.focus_candidate &&
        AttachThreadInput(GetCurrentThreadId(), target_thread, TRUE)) {
        SetActiveWindow(dialog);
        SetFocus(controls.focus_candidate);
        AttachThreadInput(GetCurrentThreadId(), target_thread, FALSE);
        info = {sizeof(info)};
        const bool after = GetGUIThreadInfo(target_thread, &info) != FALSE;
        std::printf("setup_gui=%d focus=%p capture=%p flags=%lu\n", after ? 1 : 0,
            static_cast<void*>(info.hwndFocus), static_cast<void*>(info.hwndCapture), info.flags);
    }
    if (needs_focus && (!candidate_valid || !GetGUIThreadInfo(target_thread, &info) ||
                        info.hwndFocus != controls.focus_candidate || info.hwndCapture)) {
        std::printf("fixture_focus_not_ready=1\n");
        CloseDesktop(desktop);
        return 1;
    }
    bool result = false;
    if (valid && (fault || fault_recover)) result = RunFolderLinkFault(dialog, desktop, argv[4], argv[6], argv[7], fault_recover);
    else if (valid && recover) result = RunRecovery(dialog, target_thread, desktop, argv[4]);
    else if (valid && twice) {
        const std::wstring first = argv[4];
        const auto slash = first.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const auto second = first.substr(0, slash + 1) + L"start";
            result = RunFolderLinkSequence(dialog, desktop, first, second, argv[6], controls.focus_candidate);
        }
    } else if (valid) result = pulse::app::NativeDialogFolderLinkNavigateForTest(dialog, argv[4], desktop, true);
    CloseDesktop(desktop);
    if (fault || fault_recover || twice || recover) return result ? 0 : 1;
    std::printf("issued=%d expected=%d\n", result ? 1 : 0, expect_accept ? 1 : 0);
    return result == expect_accept ? 0 : 1;
}
