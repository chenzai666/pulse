#include "native_dialog_folder_link.h"
#include "native_dialog_folder_link_policy.h"
#include "folder_source.h"

#include <UIAutomation.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace pulse::app {
using Microsoft::WRL::ComPtr;
using folder_link::WindowIdentity;

struct NativeDialogFolderLink::Shared {
    std::atomic<bool> stopping{false};
    std::atomic<DWORD> thread_id{0};
    std::wstring marker;
    HWND marked_window = nullptr;
#ifdef PULSE_WITH_SELFTEST
    bool test_allow_nonforeground = false;
    bool test_issued = false;
#endif
};

namespace {
constexpr DWORD kMessageTimeoutMs = 200;
constexpr int kAddressId = 41477;
constexpr int kToolbarId = 1001;

bool ClassIs(HWND window, const wchar_t* expected) {
    wchar_t name[96]{};
    return window && GetClassNameW(window, name, ARRAYSIZE(name)) && wcscmp(name, expected) == 0;
}

bool Identity(HWND window, WindowIdentity& identity) {
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(window, &pid);
    if (!tid || !pid || GetAncestor(window, GA_ROOT) != window) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return false;
    FILETIME created{}, ended{}, kernel{}, user{};
    const bool ok = GetProcessTimes(process, &created, &ended, &kernel, &user) &&
        WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    DWORD check_pid = 0;
    if (!ok || GetWindowThreadProcessId(window, &check_pid) != tid || check_pid != pid ||
        GetAncestor(window, GA_ROOT) != window) return false;
    identity = {reinterpret_cast<uintptr_t>(window), pid, tid,
        (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
    return identity.valid();
}

bool SameIdentity(const WindowIdentity& expected) {
    WindowIdentity actual{};
    return expected.valid() && Identity(reinterpret_cast<HWND>(expected.window), actual) && actual == expected;
}

// Restrict messages to the same logged-in user and the same integrity level.
// Access-denied, protected processes and higher/lower integrity all fail shut.
bool TokenIdentity(HANDLE process, std::vector<BYTE>& sid, DWORD& integrity) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    bool ok = size && GetTokenInformation(token, TokenUser, user.data(), size, &size);
    if (ok) {
        const auto* entry = reinterpret_cast<const TOKEN_USER*>(user.data());
        const DWORD sid_size = GetLengthSid(entry->User.Sid);
        sid.resize(sid_size);
        ok = sid_size && CopySid(sid_size, sid.data(), entry->User.Sid);
    }
    size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> level(size);
    if (ok) ok = size && GetTokenInformation(token, TokenIntegrityLevel, level.data(), size, &size);
    if (ok) {
        const auto* entry = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(level.data());
        const BYTE count = *GetSidSubAuthorityCount(entry->Label.Sid);
        ok = count != 0;
        if (ok) integrity = *GetSidSubAuthority(entry->Label.Sid, count - 1);
    }
    CloseHandle(token);
    return ok;
}

bool SameSecurity(DWORD target_pid) {
    HANDLE target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target_pid);
    if (!target) return false;
    std::vector<BYTE> ours, theirs;
    DWORD our_level = 0, their_level = 0, own_session = 0, target_session = 0;
    const bool ok = ProcessIdToSessionId(GetCurrentProcessId(), &own_session) &&
        ProcessIdToSessionId(target_pid, &target_session) && own_session == target_session &&
        TokenIdentity(GetCurrentProcess(), ours, our_level) && TokenIdentity(target, theirs, their_level) &&
        our_level == their_level && !ours.empty() && !theirs.empty() && EqualSid(ours.data(), theirs.data());
    CloseHandle(target);
    return ok;
}

bool ReadControl(HWND control, std::wstring& value) {
    wchar_t text[32768]{};
    DWORD_PTR copied = 0;
    if (!SendMessageTimeoutW(control, WM_GETTEXT, ARRAYSIZE(text), reinterpret_cast<LPARAM>(text),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, kMessageTimeoutMs, &copied) || copied >= ARRAYSIZE(text) - 1)
        return false;
    value.assign(text, static_cast<size_t>(copied));
    return true;
}

bool PulseFolder(HWND window, WindowIdentity& identity, std::wstring& folder) {
    if (!ClassIs(window, L"PulseCustomMainWindow") || !Identity(window, identity) ||
        identity.process != GetCurrentProcessId() || !GetPropW(window, kFolderSourceMarker)) return false;
    HWND control = GetDlgItem(window, kFolderSourceControl);
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(control, &pid);
    if (!control || GetParent(control) != window || !ClassIs(control, L"Edit") ||
        pid != identity.process || tid != identity.thread ||
        !(GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY) || !ReadControl(control, folder) ||
        !IsFolderSourcePath(folder) || !SameIdentity(identity) || !GetPropW(window, kFolderSourceMarker) ||
        GetDlgItem(window, kFolderSourceControl) != control) return false;
    return true;
}

bool DialogIdentity(HWND window, WindowIdentity& identity) {
    return ClassIs(window, L"#32770") && IsWindowVisible(window) && IsWindowEnabled(window) &&
        Identity(window, identity) && identity.process != GetCurrentProcessId() && SameSecurity(identity.process);
}

struct Controls { HWND dialog = nullptr; HWND band = nullptr; HWND toolbar = nullptr; HWND edit = nullptr;
                  int bands = 0, toolbars = 0, edits = 0, shell_views = 0; };
BOOL CALLBACK FindControls(HWND window, LPARAM param) {
    auto& found = *reinterpret_cast<Controls*>(param);
    if (ClassIs(window, L"DUIViewWndClassName") && IsWindowVisible(window)) ++found.shell_views;
    if (ClassIs(window, L"Address Band Root") && GetDlgCtrlID(window) == kAddressId && IsWindowVisible(window)) {
        ++found.bands; found.band = window;
    }
    return TRUE;
}
BOOL CALLBACK FindInBand(HWND window, LPARAM param) {
    auto& found = *reinterpret_cast<Controls*>(param);
    if (ClassIs(window, L"ToolbarWindow32") && GetDlgCtrlID(window) == kToolbarId && IsWindowVisible(window)) {
        ++found.toolbars; found.toolbar = window;
    }
    if (ClassIs(window, L"Edit") && GetDlgCtrlID(window) == kAddressId && IsWindowVisible(window)) {
        bool combo = false;
        for (HWND p = GetParent(window); p && p != found.band; p = GetParent(p))
            if (ClassIs(p, L"ComboBoxEx32")) combo = true;
        if (combo) { ++found.edits; found.edit = window; }
    }
    return TRUE;
}

bool FindBand(HWND dialog, Controls& found, bool require_toolbar = true) {
    found = {}; found.dialog = dialog;
    EnumChildWindows(dialog, FindControls, reinterpret_cast<LPARAM>(&found));
    if (found.bands != 1 || found.shell_views != 1 || !IsChild(dialog, found.band)) return false;
    EnumChildWindows(found.band, FindInBand, reinterpret_cast<LPARAM>(&found));
    return !require_toolbar || (found.toolbars == 1 && IsChild(found.band, found.toolbar));
}

bool SameThread(HWND child, const WindowIdentity& dialog) {
    DWORD pid = 0;
    return GetWindowThreadProcessId(child, &pid) == dialog.thread && pid == dialog.process;
}

bool IdleSince(DWORD tick) {
    LASTINPUTINFO input{sizeof(input), 0};
    return GetLastInputInfo(&input) && input.dwTime == tick;
}

bool FocusIs(DWORD thread, HWND control) {
    GUITHREADINFO info{sizeof(info)};
    return GetGUIThreadInfo(thread, &info) && info.hwndFocus == control;
}

bool Current(const WindowIdentity& dialog, const std::shared_ptr<NativeDialogFolderLink::Shared>& shared,
             DWORD input_tick) {
    HWND window = reinterpret_cast<HWND>(dialog.window);
    bool foreground = GetForegroundWindow() == window;
    bool idle = IdleSince(input_tick);
#ifdef PULSE_WITH_SELFTEST
    if (shared->test_allow_nonforeground) { foreground = true; idle = true; }
#endif
    return !shared->stopping && foreground && IsWindowVisible(window) &&
        IsWindowEnabled(window) && ClassIs(window, L"#32770") && SameIdentity(dialog) &&
        SameSecurity(dialog.process) && idle &&
        (shared->marker.empty() || (shared->marked_window == window &&
            GetPropW(window, shared->marker.c_str()) == reinterpret_cast<HANDLE>(1)));
}

bool BlankPoint(IUIAutomation* automation, HWND toolbar, POINT& point) {
    ComPtr<IUIAutomationElement> element;
    if (!automation || FAILED(automation->ElementFromHandle(toolbar, &element)) || !element) return false;
    RECT bounds{};
    CONTROLTYPEID type = 0;
    UIA_HWND handle = 0;
    int pid = 0;
    if (FAILED(element->get_CurrentBoundingRectangle(&bounds)) ||
        FAILED(element->get_CurrentControlType(&type)) || type != UIA_ToolBarControlTypeId ||
        FAILED(element->get_CurrentNativeWindowHandle(&handle)) ||
        reinterpret_cast<HWND>(handle) != toolbar ||
        FAILED(element->get_CurrentProcessId(&pid)) || pid <= 0) return false;
    DWORD native_pid = 0;
    GetWindowThreadProcessId(toolbar, &native_pid);
    if (native_pid != static_cast<DWORD>(pid) || bounds.right <= bounds.left || bounds.bottom - bounds.top < 10)
        return false;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_RawViewWalker(&walker)) || !walker) return false;
    ComPtr<IUIAutomationElement> child;
    if (FAILED(walker->GetFirstChildElement(element.Get(), &child))) return false;
    LONG right = bounds.left;
    int count = 0;
    while (child && count < 256) {
        RECT item{};
        BOOL offscreen = TRUE;
        if (FAILED(child->get_CurrentBoundingRectangle(&item)) || FAILED(child->get_CurrentIsOffscreen(&offscreen))) return false;
        if (!offscreen && item.right > item.left && item.bottom > item.top &&
            item.right > bounds.left && item.left < bounds.right && item.bottom > bounds.top && item.top < bounds.bottom)
            right = (std::max)(right, (std::min)(bounds.right, item.right));
        ++count;
        ComPtr<IUIAutomationElement> next;
        if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) return false;
        child = std::move(next);
    }
    if (child || !count || bounds.right - right < 30) return false;
    point = {right + (bounds.right - right) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
    RECT client{};
    if (!ScreenToClient(toolbar, &point) || !GetClientRect(toolbar, &client) ||
        point.x < 0 || point.y < 0 || point.x >= client.right || point.y >= client.bottom ||
        point.x > 0xffff || point.y > 0xffff) return false;
    return true;
}

bool SendTimed(HWND window, UINT message, WPARAM wp, LPARAM lp, DWORD_PTR& result) {
    return SendMessageTimeoutW(window, message, wp, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK,
        kMessageTimeoutMs, &result) != 0;
}

struct RestoreFocus {
    const WindowIdentity& dialog;
    const std::shared_ptr<NativeDialogFolderLink::Shared>& shared;
    DWORD input_tick;
    HWND band;
    HWND original_hwnd;
    IUIAutomationElement* original;
    ~RestoreFocus() {
        HWND window = reinterpret_cast<HWND>(dialog.window);
        if (!original || !Current(dialog, shared, input_tick) || !IsWindow(original_hwnd) ||
            !IsChild(window, original_hwnd) || !SameThread(original_hwnd, dialog)) return;
        GUITHREADINFO current{sizeof(current)};
        if (GetGUIThreadInfo(dialog.thread, &current) && current.hwndFocus &&
            (current.hwndFocus == original_hwnd || current.hwndFocus == band ||
             IsChild(band, current.hwndFocus))) original->SetFocus();
    }
};

struct RestoreAddress {
    const WindowIdentity& dialog;
    const std::shared_ptr<NativeDialogFolderLink::Shared>& shared;
    DWORD input_tick;
    HWND band;
    HWND edit;
    const std::wstring& attempted;
    std::wstring previous;
    bool changed = false;
    bool submitted = false;
    ~RestoreAddress() {
        if (!changed || submitted || !Current(dialog, shared, input_tick) || !IsWindow(edit) ||
            !SameThread(edit, dialog) || !IsChild(band, edit) || !ClassIs(edit, L"Edit") ||
            GetDlgCtrlID(edit) != kAddressId || !FocusIs(dialog.thread, edit)) return;
        std::wstring current;
        DWORD_PTR ignored = 0;
        if (ReadControl(edit, current) && current == attempted && Current(dialog, shared, input_tick) &&
            FocusIs(dialog.thread, edit))
            SendTimed(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(previous.c_str()), ignored);
    }
};

bool SamePhysicalFolder(const std::wstring& left, const std::wstring& right) {
    if (!IsFolderSourcePath(left) || !IsFolderSourcePath(right)) return false;
    auto normalize = [](const std::wstring& value, std::wstring& output) {
        wchar_t buffer[32768]{};
        const DWORD size = GetFullPathNameW(value.c_str(), ARRAYSIZE(buffer), buffer, nullptr);
        if (!size || size >= ARRAYSIZE(buffer)) return false;
        output.assign(buffer, size);
        while (output.size() > 3 && output.back() == L'\\') output.pop_back();
        return true;
    };
    std::wstring a, b;
    return normalize(left, a) && normalize(right, b) && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

struct ReleaseToolbar {
    const WindowIdentity& dialog;
    const std::shared_ptr<NativeDialogFolderLink::Shared>& shared;
    HWND band;
    HWND toolbar;
    bool down_attempted = false;
    bool up_sent = false;
    ~ReleaseToolbar() {
        if (!down_attempted || up_sent || !SameIdentity(dialog) || !SameSecurity(dialog.process) ||
            shared->marker.empty() || shared->marked_window != reinterpret_cast<HWND>(dialog.window) ||
            GetPropW(shared->marked_window, shared->marker.c_str()) != reinterpret_cast<HANDLE>(1) ||
            !IsWindow(toolbar) || !IsChild(band, toolbar) || !ClassIs(toolbar, L"ToolbarWindow32") ||
            GetDlgCtrlID(toolbar) != kToolbarId || !SameThread(toolbar, dialog)) return;
        GUITHREADINFO info{sizeof(info)};
        // Only release our down when this exact toolbar still owns capture.
        if (GetGUIThreadInfo(dialog.thread, &info) && info.hwndCapture == toolbar) {
            DWORD_PTR ignored = 0;
            // Release outside the client area so it cannot invoke a toolbar
            // item which has moved underneath the original blank point.
            SendTimed(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(0xffff, 0xffff), ignored);
        }
    }
};

struct ReleaseReturn {
    const WindowIdentity& dialog;
    const std::shared_ptr<NativeDialogFolderLink::Shared>& shared;
    HWND band;
    HWND edit;
    bool attempted = false;
    bool released = false;
    ~ReleaseReturn() {
        if (!attempted || released || !SameIdentity(dialog) || !SameSecurity(dialog.process) ||
            shared->marker.empty() || shared->marked_window != reinterpret_cast<HWND>(dialog.window) ||
            GetPropW(shared->marked_window, shared->marker.c_str()) != reinterpret_cast<HANDLE>(1) ||
            !IsWindow(edit) || !IsChild(band, edit) || !ClassIs(edit, L"Edit") ||
            GetDlgCtrlID(edit) != kAddressId || !SameThread(edit, dialog) || !FocusIs(dialog.thread, edit)) return;
        DWORD_PTR ignored = 0;
        SendTimed(edit, WM_KEYUP, VK_RETURN, 0, ignored);
    }
};

void Navigate(const WindowIdentity& dialog, const std::wstring& folder,
              const std::shared_ptr<NativeDialogFolderLink::Shared>& shared, IUIAutomation* automation) {
    LASTINPUTINFO input{sizeof(input), 0};
    if (!GetLastInputInfo(&input) || !Current(dialog, shared, input.dwTime) || !IsFolderSourcePath(folder)) return;
    const DWORD attributes = GetFileAttributesW(folder.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !Current(dialog, shared, input.dwTime)) return;
    HWND window = reinterpret_cast<HWND>(dialog.window);
    Controls controls;
    if (!FindBand(window, controls) || !SameThread(controls.band, dialog) ||
        !SameThread(controls.toolbar, dialog) || !IsWindowEnabled(controls.toolbar)) return;
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(dialog.thread, &info) || !info.hwndFocus || info.hwndCapture ||
        (info.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) ||
        !IsChild(window, info.hwndFocus) || !SameThread(info.hwndFocus, dialog) ||
        info.hwndFocus == controls.band || IsChild(controls.band, info.hwndFocus)) return;
    HWND original_focus = info.hwndFocus;
    ComPtr<IUIAutomationElement> original;
    if (FAILED(automation->ElementFromHandle(original_focus, &original)) || !original) return;
    POINT point{};
    if (!BlankPoint(automation, controls.toolbar, point) || !Current(dialog, shared, input.dwTime)) return;
    Controls check;
    if (!FindBand(window, check) || check.band != controls.band || check.toolbar != controls.toolbar ||
        !SameThread(check.toolbar, dialog) || !FocusIs(dialog.thread, original_focus)) return;
    POINT again{};
    if (!BlankPoint(automation, check.toolbar, again) || again.x != point.x || again.y != point.y ||
        !Current(dialog, shared, input.dwTime) || !FocusIs(dialog.thread, original_focus)) return;
    RestoreFocus restore{dialog, shared, input.dwTime, controls.band, original_focus, original.Get()};
    DWORD_PTR ignored = 0;
    const LPARAM position = MAKELPARAM(point.x, point.y);
    ReleaseToolbar release{dialog, shared, controls.band, check.toolbar};
    release.down_attempted = true;
    if (!SendTimed(check.toolbar, WM_LBUTTONDOWN, MK_LBUTTON, position, ignored)) return;
    // Mouse-up is directed to the same verified toolbar, even if the down
    // handler changed focus. Never send a message to a replacement window.
    if (!Current(dialog, shared, input.dwTime) || !SameThread(check.toolbar, dialog) ||
        !IsChild(check.band, check.toolbar) || !ClassIs(check.toolbar, L"ToolbarWindow32")) return;
    if (!SendTimed(check.toolbar, WM_LBUTTONUP, 0, position, ignored)) return;
    release.up_sent = true;
    if (!Current(dialog, shared, input.dwTime) || !FindBand(window, check, false) || check.band != controls.band ||
        check.edits != 1 || !SameThread(check.edit, dialog) || !IsWindowEnabled(check.edit) ||
        !FocusIs(dialog.thread, check.edit)) return;
    HWND edit = check.edit;
    RestoreAddress restore_address{dialog, shared, input.dwTime, controls.band, edit, folder};
    if (!ReadControl(edit, restore_address.previous) || !Current(dialog, shared, input.dwTime) ||
        !FocusIs(dialog.thread, edit)) return;
    if (SamePhysicalFolder(restore_address.previous, folder)) return;
    if (!SendTimed(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(folder.c_str()), ignored) || !ignored) return;
    restore_address.changed = true;
    std::wstring text;
    if (!Current(dialog, shared, input.dwTime) || !FindBand(window, check, false) || check.edit != edit ||
        check.edits != 1 || !SameThread(edit, dialog) || !FocusIs(dialog.thread, edit) ||
        !ReadControl(edit, text) || text != folder || !Current(dialog, shared, input.dwTime) ||
        !FocusIs(dialog.thread, edit)) return;
    // Only the proven, focused Address Band edit receives Enter. No global
    // input and no WM_COMMAND / IDOK are ever sent by Pulse.
    ReleaseReturn release_return{dialog, shared, controls.band, edit};
    release_return.attempted = true;
    if (!SendTimed(edit, WM_KEYDOWN, VK_RETURN, 0, ignored)) return;
    restore_address.submitted = true;
#ifdef PULSE_WITH_SELFTEST
    shared->test_issued = true;
#endif
    if (Current(dialog, shared, input.dwTime) && IsWindow(edit) && SameThread(edit, dialog) &&
        ClassIs(edit, L"Edit") && GetDlgCtrlID(edit) == kAddressId && IsChild(controls.band, edit) &&
        FocusIs(dialog.thread, edit))
        release_return.released = SendTimed(edit, WM_KEYUP, VK_RETURN, 0, ignored);
}

struct Watcher {
    std::shared_ptr<NativeDialogFolderLink::Shared> shared;
    folder_link::Transition transition;
    WindowIdentity last_dialog{}, pulse{};
    ComPtr<IUIAutomation> automation;
    HWINEVENTHOOK foreground = nullptr, destroyed = nullptr;

    void ClearMarker() {
        if (shared->marked_window && SameIdentity(last_dialog) &&
            GetPropW(shared->marked_window, shared->marker.c_str()) == reinterpret_cast<HANDLE>(1))
            RemovePropW(shared->marked_window, shared->marker.c_str());
        shared->marked_window = nullptr;
    }

    void OnForeground(HWND window, DWORD event_time) {
        if (shared->stopping || window != GetForegroundWindow() || GetTickCount() - event_time > 500) {
            transition.OtherForeground(); return;
        }
        WindowIdentity identity{};
        std::wstring folder;
        if (PulseFolder(window, identity, folder)) {
            pulse = identity;
            transition.PulseForeground(identity, std::move(folder));
            return;
        }
        if (!DialogIdentity(window, identity)) { transition.OtherForeground(); return; }
        if (!(identity == last_dialog) || shared->marked_window != window) {
            ClearMarker();
            Controls controls;
            if (!FindBand(window, controls) || !SameThread(controls.band, identity) ||
                !SameThread(controls.toolbar, identity) || shared->marker.empty() ||
                !SameIdentity(identity) || !SetPropW(window, shared->marker.c_str(), reinterpret_cast<HANDLE>(1))) {
                transition.OtherForeground(); last_dialog = {}; return;
            }
            shared->marked_window = window;
            last_dialog = identity;
        } else if (GetPropW(window, shared->marker.c_str()) != reinterpret_cast<HANDLE>(1)) {
            transition.OtherForeground(); shared->marked_window = nullptr; last_dialog = {}; return;
        }
        transition.DialogForeground(identity);
        WindowIdentity current_pulse{};
        if (pulse.valid() && PulseFolder(reinterpret_cast<HWND>(pulse.window), current_pulse, folder)) {
            if (transition.Consume(identity, current_pulse, folder)) Navigate(identity, folder, shared, automation.Get());
        }
        last_dialog = identity;
        // Preserve this dialog as the beginning of the next transition even
        // after an attempted navigation consumed the preceding sequence.
        transition.DialogForeground(identity);
    }
    void OnDestroy(HWND window) {
        if (reinterpret_cast<uintptr_t>(window) == last_dialog.window ||
            reinterpret_cast<uintptr_t>(window) == pulse.window) {
            transition.OtherForeground();
            ClearMarker();
            last_dialog = {}; pulse = {};
        }
    }
};
thread_local Watcher* active_watcher = nullptr;
void CALLBACK EventCallback(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD time) {
    if (!active_watcher || !window || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    if (event == EVENT_SYSTEM_FOREGROUND) active_watcher->OnForeground(window, time);
    else if (event == EVENT_OBJECT_DESTROY) active_watcher->OnDestroy(window);
}
DWORD WINAPI ThreadMain(void* context) {
    std::unique_ptr<std::shared_ptr<NativeDialogFolderLink::Shared>> holder(
        static_cast<std::shared_ptr<NativeDialogFolderLink::Shared>*>(context));
    auto shared = *holder;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    shared->thread_id = GetCurrentThreadId();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 0;
    Watcher watcher{shared};
    GUID marker_id{};
    wchar_t marker_text[64]{};
    if (SUCCEEDED(CoCreateGuid(&marker_id)) && StringFromGUID2(marker_id, marker_text, ARRAYSIZE(marker_text)))
        shared->marker = std::wstring(L"Pulse.FolderLink.") + marker_text;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&watcher.automation))) && watcher.automation) {
        active_watcher = &watcher;
        watcher.foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                            EventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
        watcher.destroyed = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr,
                                           EventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (watcher.foreground && watcher.destroyed && !shared->stopping) {
            watcher.OnForeground(GetForegroundWindow(), GetTickCount());
            while (!shared->stopping && GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        if (watcher.destroyed) UnhookWinEvent(watcher.destroyed);
        if (watcher.foreground) UnhookWinEvent(watcher.foreground);
        watcher.ClearMarker();
        active_watcher = nullptr;
    }
    watcher.automation.Reset();
    CoUninitialize();
    return 0;
}
} // namespace

#ifdef PULSE_WITH_SELFTEST
bool NativeDialogFolderLinkNavigateForTest(HWND dialog, const std::wstring& folder,
                                            HDESK desktop, bool allow_nonforeground) {
    if (!desktop || !SetThreadDesktop(desktop)) return false;
    WindowIdentity identity{};
    if (!DialogIdentity(dialog, identity)) return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return false;
    ComPtr<IUIAutomation> automation;
    auto shared = std::make_shared<NativeDialogFolderLink::Shared>();
    shared->test_allow_nonforeground = allow_nonforeground;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&automation))) && automation)
        Navigate(identity, folder, shared, automation.Get());
    automation.Reset();
    CoUninitialize();
    return shared->test_issued;
}
#endif


NativeDialogFolderLink::NativeDialogFolderLink() : shared_(std::make_shared<Shared>()) {
    auto* context = new std::shared_ptr<Shared>(shared_);
    thread_ = CreateThread(nullptr, 0, ThreadMain, context, 0, nullptr);
    if (!thread_) { delete context; shared_->stopping = true; }
}
NativeDialogFolderLink::~NativeDialogFolderLink() {
    Stop();
    // Final process shutdown must not tear down COM/CRT while the watcher is
    // still executing. Settings changes keep the object alive on a timeout.
    if (thread_) { WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); thread_ = nullptr; }
}
bool NativeDialogFolderLink::Stop() noexcept {
    if (!shared_) return true;
    shared_->stopping = true;
    const DWORD id = shared_->thread_id.load();
    if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
    if (thread_) {
        if (WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0) return false;
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    shared_.reset();
    return true;
}
bool NativeDialogFolderLink::IsStopping() const noexcept { return !shared_ || shared_->stopping.load(); }
} // namespace pulse::app
