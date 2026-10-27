#include "native_dialog_folder_link.h"
#include "native_dialog_folder_link_policy.h"
#include "folder_source.h"

// WIN32_LEAN_AND_MEAN omits COM declarations from windows.h. Include COM
// explicitly before the generated UI Automation interface declarations.
#include <objbase.h>
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

// Shared with the owning parent process. The helper publishes immutable
// identity and text before publishing a stage with an interlocked operation.
// The parent consumes it only after the helper process is signaled.
struct RecoveryRecord {
    volatile LONG64 deadline = 0;
    volatile LONG stage = 0;
    volatile LONG cleanup_claim = 0;
    DWORD supervisor_pid = 0;
    uint64_t supervisor_created = 0;
    DWORD pid = 0, thread = 0, input_tick = 0;
    uint64_t created = 0;
    uintptr_t dialog = 0, band = 0, toolbar = 0, edit = 0, original_focus = 0;
    wchar_t original_text[32768]{};
    wchar_t attempted_text[32768]{};
};

struct NativeDialogFolderLink::Shared {
    std::atomic<bool> stopping{false};
    std::atomic<DWORD> thread_id{0};
    DWORD owner_pid = 0;
    HANDLE owner = nullptr;
    HANDLE stop_event = nullptr;
    HANDLE supervisor = nullptr;
    volatile LONG64* deadline = nullptr;
    RecoveryRecord* recovery = nullptr;
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
// The SDK hides PROC_THREAD_ATTRIBUTE_JOB_LIST when _WIN32_WINNT is below
// Windows 10. Its documented value is attribute 13 with the input flag; use
// the same encoding so CreateProcess assigns the job atomically. On older
// Windows UpdateProcThreadAttribute fails and the helper remains disabled.
#ifdef PROC_THREAD_ATTRIBUTE_JOB_LIST
constexpr DWORD_PTR kJobListAttribute = PROC_THREAD_ATTRIBUTE_JOB_LIST;
#else
constexpr DWORD_PTR kJobListAttribute = ProcThreadAttributeValue(13, FALSE, TRUE, FALSE);
#endif

// Fixed event names and numeric OS status only: never emit user paths or file
// names. DebugView/ETW consumers can observe early child startup failures.
void TraceFolderLink(const wchar_t* event, DWORD status = 0) {
    wchar_t line[256]{};
    swprintf_s(line, L"Pulse.NativeFolderLink event=%s pid=%lu status=%lu\n",
               event, GetCurrentProcessId(), status);
    OutputDebugStringW(line);
}

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

bool PulseFolder(HWND window, DWORD owner_pid, WindowIdentity& identity, std::wstring& folder) {
    if (!ClassIs(window, L"PulseCustomMainWindow") || !Identity(window, identity) ||
        identity.process != owner_pid || !GetPropW(window, kFolderSourceMarker)) return false;
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
    if (shared->stopping || !IsWindowVisible(window) || !IsWindowEnabled(window) ||
        (shared->stop_event && WaitForSingleObject(shared->stop_event, 0) != WAIT_TIMEOUT) ||
        (shared->owner && WaitForSingleObject(shared->owner, 0) != WAIT_TIMEOUT) ||
        (shared->supervisor && WaitForSingleObject(shared->supervisor, 0) != WAIT_TIMEOUT) ||
        !ClassIs(window, L"#32770") || !SameIdentity(dialog) || !SameSecurity(dialog.process) ||
        (!shared->marker.empty() && (shared->marked_window != window ||
            GetPropW(window, shared->marker.c_str()) != reinterpret_cast<HANDLE>(1)))) return false;
#ifdef PULSE_WITH_SELFTEST
    if (shared->test_allow_nonforeground) return true;
#endif
    return !shared->stopping && GetForegroundWindow() == window && IdleSince(input_tick);
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
    DWORD input_tick;
    HWND band;
    HWND toolbar;
    bool down_attempted = false;
    bool up_sent = false;
    ~ReleaseToolbar() {
        HWND window = reinterpret_cast<HWND>(dialog.window);
        if (!down_attempted || up_sent || !Current(dialog, shared, input_tick) ||
            shared->marker.empty() || shared->marked_window != reinterpret_cast<HWND>(dialog.window) ||
            GetPropW(shared->marked_window, shared->marker.c_str()) != reinterpret_cast<HANDLE>(1) ||
            !IsChild(window, band) || !ClassIs(band, L"Address Band Root") ||
            GetDlgCtrlID(band) != kAddressId || !SameThread(band, dialog) ||
            !IsWindow(toolbar) || !IsChild(band, toolbar) || !ClassIs(toolbar, L"ToolbarWindow32") ||
            GetDlgCtrlID(toolbar) != kToolbarId || !SameThread(toolbar, dialog)) return;
        GUITHREADINFO info{sizeof(info)};
        // Only release our down when this exact toolbar still owns capture.
        if (GetGUIThreadInfo(dialog.thread, &info) && info.hwndCapture == toolbar &&
            Current(dialog, shared, input_tick) && IsChild(window, band) && IsChild(band, toolbar) &&
            ClassIs(band, L"Address Band Root") && GetDlgCtrlID(band) == kAddressId && SameThread(band, dialog) &&
            ClassIs(toolbar, L"ToolbarWindow32") && GetDlgCtrlID(toolbar) == kToolbarId && SameThread(toolbar, dialog)) {
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
    DWORD input_tick;
    HWND band;
    HWND edit;
    bool attempted = false;
    bool released = false;
    ~ReleaseReturn() {
        HWND window = reinterpret_cast<HWND>(dialog.window);
        if (!attempted || released || !Current(dialog, shared, input_tick) ||
            shared->marker.empty() || shared->marked_window != reinterpret_cast<HWND>(dialog.window) ||
            GetPropW(shared->marked_window, shared->marker.c_str()) != reinterpret_cast<HANDLE>(1) ||
            !IsChild(window, band) || !ClassIs(band, L"Address Band Root") ||
            GetDlgCtrlID(band) != kAddressId || !SameThread(band, dialog) ||
            !IsWindow(edit) || !IsChild(band, edit) || !ClassIs(edit, L"Edit") ||
            GetDlgCtrlID(edit) != kAddressId || !SameThread(edit, dialog) || !FocusIs(dialog.thread, edit)) return;
        if (!Current(dialog, shared, input_tick) || !IsChild(window, band) || !IsChild(band, edit) ||
            !ClassIs(band, L"Address Band Root") || GetDlgCtrlID(band) != kAddressId || !SameThread(band, dialog) ||
            !ClassIs(edit, L"Edit") || GetDlgCtrlID(edit) != kAddressId || !SameThread(edit, dialog) ||
            !FocusIs(dialog.thread, edit)) return;
        DWORD_PTR ignored = 0;
        SendTimed(edit, WM_KEYUP, VK_RETURN, 0, ignored);
    }
};

struct RecoverySession {
    RecoveryRecord* record;
    bool complete = false;
    explicit RecoverySession(RecoveryRecord* value) : record(value) {}
    ~RecoverySession() { if (record && complete) InterlockedExchange(&record->stage, 0); }
    void Publish(LONG stage) { if (record) InterlockedExchange(&record->stage, stage); }
};

void Navigate(const WindowIdentity& dialog, const std::wstring& folder,
              const std::shared_ptr<NativeDialogFolderLink::Shared>& shared, IUIAutomation* automation) {
    LASTINPUTINFO input{sizeof(input), 0};
    if ((shared->recovery && InterlockedCompareExchange(&shared->recovery->stage, 0, 0) != 0) ||
        !GetLastInputInfo(&input) || !Current(dialog, shared, input.dwTime) || !IsFolderSourcePath(folder)) return;
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
    RecoverySession recovery(shared->recovery);
    if (auto* r = shared->recovery) {
        r->pid = dialog.process; r->thread = dialog.thread; r->created = dialog.process_created;
        r->input_tick = input.dwTime; r->dialog = dialog.window;
        r->band = reinterpret_cast<uintptr_t>(controls.band);
        r->toolbar = reinterpret_cast<uintptr_t>(check.toolbar);
        r->edit = 0; r->original_focus = reinterpret_cast<uintptr_t>(original_focus);
        r->original_text[0] = L'\0'; r->attempted_text[0] = L'\0';
    }
    RestoreFocus restore{dialog, shared, input.dwTime, controls.band, original_focus, original.Get()};
    DWORD_PTR ignored = 0;
    const LPARAM position = MAKELPARAM(point.x, point.y);
    ReleaseToolbar release{dialog, shared, input.dwTime, controls.band, check.toolbar};
    release.down_attempted = true;
    recovery.Publish(1);
    if (!SendTimed(check.toolbar, WM_LBUTTONDOWN, MK_LBUTTON, position, ignored)) return;
    // Mouse-up is directed to the same verified toolbar, even if the down
    // handler changed focus. Never send a message to a replacement window.
    if (!Current(dialog, shared, input.dwTime) || !SameThread(check.toolbar, dialog) ||
        !IsChild(check.band, check.toolbar) || !ClassIs(check.toolbar, L"ToolbarWindow32")) return;
    if (!SendTimed(check.toolbar, WM_LBUTTONUP, 0, position, ignored)) return;
    release.up_sent = true;
    recovery.Publish(2);
    if (!Current(dialog, shared, input.dwTime) || !FindBand(window, check, false) || check.band != controls.band ||
        check.edits != 1 || !SameThread(check.edit, dialog) || !IsWindowEnabled(check.edit) ||
        !FocusIs(dialog.thread, check.edit)) return;
    HWND edit = check.edit;
    RestoreAddress restore_address{dialog, shared, input.dwTime, controls.band, edit, folder};
    if (!ReadControl(edit, restore_address.previous) || !Current(dialog, shared, input.dwTime) ||
        !FocusIs(dialog.thread, edit)) return;
    if (SamePhysicalFolder(restore_address.previous, folder)) { recovery.complete = true; return; }
    if (auto* r = shared->recovery) {
        r->edit = reinterpret_cast<uintptr_t>(edit);
        if (wcscpy_s(r->original_text, restore_address.previous.c_str()) ||
            wcscpy_s(r->attempted_text, folder.c_str())) return;
    }
    recovery.Publish(3);
    if (!SendTimed(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(folder.c_str()), ignored) || !ignored) return;
    restore_address.changed = true;
    recovery.Publish(4);
    std::wstring text;
    if (!Current(dialog, shared, input.dwTime) || !FindBand(window, check, false) || check.edit != edit ||
        check.edits != 1 || !SameThread(edit, dialog) || !FocusIs(dialog.thread, edit) ||
        !ReadControl(edit, text) || text != folder || !Current(dialog, shared, input.dwTime) ||
        !FocusIs(dialog.thread, edit)) return;
    // Only the proven, focused Address Band edit receives Enter. No global
    // input and no WM_COMMAND / IDOK are ever sent by Pulse.
    ReleaseReturn release_return{dialog, shared, input.dwTime, controls.band, edit};
    release_return.attempted = true;
    recovery.Publish(5);
    if (!SendTimed(edit, WM_KEYDOWN, VK_RETURN, 0, ignored)) return;
    restore_address.submitted = true;
    recovery.Publish(6);
#ifdef PULSE_WITH_SELFTEST
    shared->test_issued = true;
#endif
    if (Current(dialog, shared, input.dwTime) && IsWindow(edit) && SameThread(edit, dialog) &&
        ClassIs(edit, L"Edit") && GetDlgCtrlID(edit) == kAddressId && IsChild(controls.band, edit) &&
        FocusIs(dialog.thread, edit))
        release_return.released = SendTimed(edit, WM_KEYUP, VK_RETURN, 0, ignored);
    // A successful key-down is the navigation commit. The editor may be
    // destroyed immediately; never retain an uncommitted recovery stage for
    // that completed operation. ReleaseReturn still pairs key-up when safe.
    recovery.complete = true;
}

struct Watcher {
    std::shared_ptr<NativeDialogFolderLink::Shared> shared;
    folder_link::Transition transition;
    WindowIdentity last_dialog{}, pulse{};
    ComPtr<IUIAutomation> automation;
    HWINEVENTHOOK foreground = nullptr, destroyed = nullptr;

    void ClearMarker() {
        if (shared->recovery && InterlockedCompareExchange(&shared->recovery->stage, 0, 0) != 0) return;
        if (shared->marked_window && SameIdentity(last_dialog) &&
            GetPropW(shared->marked_window, shared->marker.c_str()) == reinterpret_cast<HANDLE>(1))
            RemovePropW(shared->marked_window, shared->marker.c_str());
        shared->marked_window = nullptr;
    }

    void OnForeground(HWND window, DWORD event_time) {
        struct Deadline {
            volatile LONG64* value;
            explicit Deadline(volatile LONG64* p) : value(p) {
                if (value) InterlockedExchange64(value, static_cast<LONG64>(GetTickCount64() + 5000));
            }
            ~Deadline() { if (value) InterlockedExchange64(value, 0); }
        } deadline(shared->deadline);
        if ((shared->recovery && InterlockedCompareExchange(&shared->recovery->stage, 0, 0) != 0) ||
            (shared->supervisor && WaitForSingleObject(shared->supervisor, 0) != WAIT_TIMEOUT) ||
            (shared->owner && WaitForSingleObject(shared->owner, 0) != WAIT_TIMEOUT) ||
            shared->stopping || (shared->stop_event && WaitForSingleObject(shared->stop_event, 0) != WAIT_TIMEOUT) ||
            window != GetForegroundWindow() || GetTickCount() - event_time > 500) {
            transition.OtherForeground(); return;
        }
        WindowIdentity identity{};
        std::wstring folder;
        if (PulseFolder(window, shared->owner_pid, identity, folder)) {
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
                !SameIdentity(identity) ||
                (shared->stop_event && WaitForSingleObject(shared->stop_event, 0) != WAIT_TIMEOUT) ||
                (shared->supervisor && WaitForSingleObject(shared->supervisor, 0) != WAIT_TIMEOUT) ||
                (shared->owner && WaitForSingleObject(shared->owner, 0) != WAIT_TIMEOUT) ||
                !SetPropW(window, shared->marker.c_str(), reinterpret_cast<HANDLE>(1))) {
                transition.OtherForeground(); last_dialog = {}; return;
            }
            shared->marked_window = window;
            last_dialog = identity;
            if ((shared->stop_event && WaitForSingleObject(shared->stop_event, 0) != WAIT_TIMEOUT) ||
                (shared->supervisor && WaitForSingleObject(shared->supervisor, 0) != WAIT_TIMEOUT) ||
                (shared->owner && WaitForSingleObject(shared->owner, 0) != WAIT_TIMEOUT)) {
                ClearMarker(); transition.OtherForeground(); last_dialog = {}; return;
            }
        } else if (GetPropW(window, shared->marker.c_str()) != reinterpret_cast<HANDLE>(1)) {
            transition.OtherForeground(); shared->marked_window = nullptr; last_dialog = {}; return;
        }
        transition.DialogForeground(identity);
        WindowIdentity current_pulse{};
        if (pulse.valid() && PulseFolder(reinterpret_cast<HWND>(pulse.window), shared->owner_pid, current_pulse, folder)) {
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
    if (event == EVENT_SYSTEM_FOREGROUND) {
        active_watcher->OnForeground(window, time);
        if (active_watcher->shared->recovery &&
            InterlockedCompareExchange(&active_watcher->shared->recovery->stage, 0, 0) != 0)
            PostQuitMessage(0); // let the parent recover after this process exits
    }
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
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&watcher.automation))) && watcher.automation) {
        active_watcher = &watcher;
        watcher.foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                            EventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
        watcher.destroyed = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr,
                                           EventCallback, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (watcher.foreground && watcher.destroyed && !shared->stopping) {
            watcher.OnForeground(GetForegroundWindow(), GetTickCount());
            if (shared->deadline) InterlockedExchange64(shared->deadline, 0);
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
bool IsPrivateTestDesktop(HDESK desktop) {
    wchar_t name[256]{};
    DWORD bytes = 0;
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &bytes) &&
        std::wstring_view(name).starts_with(L"PulseFolderProbe_");
}
bool NativeDialogFolderLinkNavigateForTest(HWND dialog, const std::wstring& folder,
                                            HDESK desktop, bool allow_nonforeground) {
    if (!IsPrivateTestDesktop(desktop) ||
        (GetThreadDesktop(GetCurrentThreadId()) != desktop && !SetThreadDesktop(desktop))) return false;
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

NativeDialogFolderLinkTestResult NativeDialogFolderLinkNavigateTwiceForTest(
    HWND dialog, const std::wstring& first, const std::wstring& second, HDESK desktop) {
    NativeDialogFolderLinkTestResult result;
    if (!IsPrivateTestDesktop(desktop) ||
        (GetThreadDesktop(GetCurrentThreadId()) != desktop && !SetThreadDesktop(desktop))) return result;
    WindowIdentity identity{};
    if (!DialogIdentity(dialog, identity)) return result;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return result;
    ComPtr<IUIAutomation> automation;
    auto shared = std::make_shared<NativeDialogFolderLink::Shared>();
    auto record = std::make_unique<RecoveryRecord>();
    shared->recovery = record.get();
    shared->test_allow_nonforeground = true;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&automation))) && automation) {
        Navigate(identity, first, shared, automation.Get());
        result.first_issued = shared->test_issued;
        result.first_stage = InterlockedCompareExchange(&record->stage, 0, 0);
        shared->test_issued = false;
        Navigate(identity, second, shared, automation.Get());
        result.second_issued = shared->test_issued;
        result.second_stage = InterlockedCompareExchange(&record->stage, 0, 0);
    }
    automation.Reset();
    CoUninitialize();
    return result;
}
#endif



namespace {
struct ParentWait { HANDLE parent = nullptr; HANDLE stop = nullptr; HANDLE supervisor = nullptr; HANDLE finished = nullptr;
    DWORD thread = 0; volatile LONG64* deadline = nullptr; };
DWORD WINAPI WaitForParent(void* context) {
    const auto* wait = static_cast<ParentWait*>(context);
    HANDLE handles[] = {wait->parent, wait->stop, wait->supervisor};
    DWORD reason = WAIT_TIMEOUT;
    while ((reason = WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, 250)) == WAIT_TIMEOUT) {
        const LONG64 deadline = InterlockedCompareExchange64(wait->deadline, 0, 0);
        if (deadline > 0 && GetTickCount64() >= static_cast<ULONGLONG>(deadline)) {
            TraceFolderLink(L"helper_operation_timeout");
            TerminateProcess(GetCurrentProcess(), 4); // the owned helper, never a third-party process
        }
    }
    TraceFolderLink(L"helper_shutdown_requested", reason == WAIT_FAILED ? GetLastError() : reason);
    if (!PostThreadMessageW(wait->thread, WM_QUIT, 0, 0))
        TraceFolderLink(L"helper_quit_post_failed", GetLastError());
    // The watcher can be stuck inside an external UIA or network call. Keep
    // this watchdog alive after supervisor/parent loss instead of abandoning
    // the process; after the grace period only this disposable helper is killed.
    if (WaitForSingleObject(wait->finished, 1500) != WAIT_OBJECT_0) {
        TraceFolderLink(L"helper_shutdown_timeout");
        TerminateProcess(GetCurrentProcess(), 6);
    }
    return 0;
}
bool SameExecutable(HANDLE parent) {
    wchar_t own[32768]{}, other[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, own, ARRAYSIZE(own));
    DWORD other_size = ARRAYSIZE(other);
    return size && size < ARRAYSIZE(own) && QueryFullProcessImageNameW(parent, 0, other, &other_size) &&
        _wcsicmp(own, other) == 0;
}
bool IsChildOf(HANDLE process, DWORD expected_pid) {
    struct BasicInfo {
        LONG exit_status;
        void* peb;
        ULONG_PTR affinity;
        LONG priority;
        ULONG_PTR process_id;
        ULONG_PTR parent_id;
    } info{};
    using Query = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "NtQueryInformationProcess")) : nullptr;
    return query && query(process, 0, &info, sizeof(info), nullptr) >= 0 &&
        info.process_id == GetProcessId(process) && info.parent_id == expected_pid;
}
bool IsActualParent(DWORD expected_pid) { return IsChildOf(GetCurrentProcess(), expected_pid); }
uint64_t ProcessCreated(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
void CloseOwned(HANDLE& value) { if (value) { CloseHandle(value); value = nullptr; } }
BOOL CALLBACK RemoveOwnMarker(HWND window, LPARAM value) {
    const auto* marker = reinterpret_cast<const std::wstring*>(value);
    if (!marker->empty() && GetPropW(window, marker->c_str()) == reinterpret_cast<HANDLE>(1))
        RemovePropW(window, marker->c_str());
    return TRUE;
}
void CleanupMarker(const std::wstring& marker) {
    if (!marker.empty()) EnumWindows(RemoveOwnMarker, reinterpret_cast<LPARAM>(&marker));
}
void RecoverInterrupted(const std::wstring& marker, RecoveryRecord* record, bool test_nonforeground = false) {
    if (!record) return;
    const LONG stage = InterlockedCompareExchange(&record->stage, 0, 0);
    if (stage <= 0) return;
    WindowIdentity identity{record->dialog, record->pid, record->thread, record->created};
    HWND window = reinterpret_cast<HWND>(record->dialog);
    HWND band = reinterpret_cast<HWND>(record->band);
    HWND toolbar = reinterpret_cast<HWND>(record->toolbar);
    HWND edit = reinterpret_cast<HWND>(record->edit);
    const auto foreground = [window, test_nonforeground] {
#ifdef PULSE_WITH_SELFTEST
        if (test_nonforeground) return true;
#else
        (void)test_nonforeground;
#endif
        return GetForegroundWindow() == window;
    };
    const auto valid_target = [&] {
        return SameIdentity(identity) && SameSecurity(identity.process) && foreground() &&
            IsWindowVisible(window) && IsWindowEnabled(window) && IdleSince(record->input_tick) &&
            GetPropW(window, marker.c_str()) == reinterpret_cast<HANDLE>(1) &&
            IsChild(window, band) && ClassIs(band, L"Address Band Root") &&
            GetDlgCtrlID(band) == kAddressId && SameThread(band, identity);
    };
    if (!valid_target()) return;
    if (stage == 1 && IsChild(band, toolbar) && ClassIs(toolbar, L"ToolbarWindow32") &&
        GetDlgCtrlID(toolbar) == kToolbarId && SameThread(toolbar, identity)) {
        if (valid_target() && IsChild(band, toolbar) && ClassIs(toolbar, L"ToolbarWindow32") &&
            GetDlgCtrlID(toolbar) == kToolbarId && SameThread(toolbar, identity)) {
            DWORD_PTR ignored = 0;
            SendTimed(toolbar, WM_LBUTTONUP, 0, MAKELPARAM(0xffff, 0xffff), ignored);
        }
    }
    // Stage 5 means Return may already have been delivered; never roll back
    // that address. At stages 3/4 restore only our exact unsubmitted text.
    if ((stage == 3 || stage == 4) && IsChild(band, edit) && ClassIs(edit, L"Edit") &&
        GetDlgCtrlID(edit) == kAddressId && SameThread(edit, identity) && FocusIs(identity.thread, edit)) {
        std::wstring current;
        if (ReadControl(edit, current) && current == record->attempted_text &&
            valid_target() && IsChild(band, edit) && ClassIs(edit, L"Edit") &&
            GetDlgCtrlID(edit) == kAddressId && SameThread(edit, identity) && FocusIs(identity.thread, edit)) {
            DWORD_PTR ignored = 0;
            SendTimed(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(record->original_text), ignored);
        }
    }
}
bool StartSupervisor(HANDLE parent, HANDLE helper, HANDLE mapping, HANDLE ready, HANDLE done,
                     const std::wstring& marker, HANDLE& supervisor) {
    // Each handle is explicitly duplicated as inheritable. No job handle is
    // inherited, so a parent crash still closes the helper's kill-on-close job.
    HANDLE inherited[5]{};
    HANDLE originals[] = {parent, helper, mapping, ready, done};
    const DWORD access[] = {SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FILE_MAP_ALL_ACCESS,
        EVENT_MODIFY_STATE, EVENT_MODIFY_STATE};
    const HANDLE self = GetCurrentProcess();
    for (size_t i = 0; i < ARRAYSIZE(inherited); ++i) {
        if (!DuplicateHandle(self, originals[i], self, &inherited[i], access[i], TRUE, 0)) {
            for (HANDLE& handle : inherited) CloseOwned(handle);
            return false;
        }
    }
    wchar_t executable[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!size || size >= ARRAYSIZE(executable) || !bytes ||
        !InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
        for (HANDLE& handle : inherited) CloseOwned(handle);
        return false;
    }
    const bool configured = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                       inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributes;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --native-folder-link-supervisor";
    for (HANDLE handle : inherited) command += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(handle));
    command += L" " + marker;
    PROCESS_INFORMATION child{};
    const bool created = configured && CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(attributes);
    for (HANDLE& handle : inherited) CloseOwned(handle);
    if (!created) return false;
    CloseHandle(child.hThread);
    supervisor = child.hProcess;
    return true;
}
struct FallbackTask { HANDLE helper = nullptr, supervisor = nullptr, mapping = nullptr, done = nullptr;
    std::wstring marker; };
DWORD WINAPI CleanupFallback(void* context) {
    std::unique_ptr<FallbackTask> task(static_cast<FallbackTask*>(context));
    HANDLE processes[] = {task->helper, task->supervisor};
    if (WaitForMultipleObjects(ARRAYSIZE(processes), processes, TRUE, INFINITE) == WAIT_OBJECT_0 &&
        WaitForSingleObject(task->done, 0) == WAIT_TIMEOUT) {
        auto* record = static_cast<RecoveryRecord*>(MapViewOfFile(task->mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                                                  sizeof(RecoveryRecord)));
        if (record) {
            // A claim of 1 means the supervisor may have been interrupted in
            // a cross-process message. Its outcome is unknown: do not replay.
            if (InterlockedCompareExchange(&record->cleanup_claim, 1, 0) == 0)
                RecoverInterrupted(task->marker, record);
            CleanupMarker(task->marker);
            InterlockedExchange(&record->cleanup_claim, 2);
            UnmapViewOfFile(record);
        }
        SetEvent(task->done);
    }
    CloseHandle(task->helper); CloseHandle(task->supervisor);
    CloseHandle(task->mapping); CloseHandle(task->done);
    return 0;
}
bool StartFallback(HANDLE helper, HANDLE supervisor, HANDLE mapping, HANDLE done, const std::wstring& marker) {
    auto task = std::make_unique<FallbackTask>();
    task->marker = marker;
    const HANDLE self = GetCurrentProcess();
    if (!DuplicateHandle(self, helper, self, &task->helper, SYNCHRONIZE, FALSE, 0) ||
        !DuplicateHandle(self, supervisor, self, &task->supervisor, SYNCHRONIZE, FALSE, 0) ||
        !DuplicateHandle(self, mapping, self, &task->mapping, FILE_MAP_ALL_ACCESS, FALSE, 0) ||
        !DuplicateHandle(self, done, self, &task->done, SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, 0)) {
        CloseOwned(task->helper); CloseOwned(task->supervisor); CloseOwned(task->mapping); CloseOwned(task->done);
        return false;
    }
    HANDLE thread = CreateThread(nullptr, 0, CleanupFallback, task.get(), 0, nullptr);
    if (!thread) {
        CloseOwned(task->helper); CloseOwned(task->supervisor); CloseOwned(task->mapping); CloseOwned(task->done);
        return false;
    }
    task.release();
    CloseHandle(thread);
    return true;
}
DWORD WINAPI RecoveryWatchdog(void* context) {
    HANDLE finished = static_cast<HANDLE>(context);
    if (WaitForSingleObject(finished, 3000) == WAIT_TIMEOUT)
        TerminateProcess(GetCurrentProcess(), 5); // only this disposable supervisor
    return 0;
}
}

#ifdef PULSE_WITH_SELFTEST
bool NativeDialogFolderLinkRecoverForTest(HWND dialog, HWND band, HWND toolbar, HWND edit,
    LONG stage, const std::wstring& previous, const std::wstring& attempted, HDESK desktop) {
    if (!IsPrivateTestDesktop(desktop) ||
        (GetThreadDesktop(GetCurrentThreadId()) != desktop && !SetThreadDesktop(desktop)) ||
        stage < 1 || stage > 6) return false;
    WindowIdentity identity{};
    if (!DialogIdentity(dialog, identity) || !IsChild(dialog, band) || !SameThread(band, identity)) return false;
    GUID id{};
    wchar_t text[64]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, text, ARRAYSIZE(text))) return false;
    const std::wstring marker = std::wstring(L"Pulse.FolderLink.") + text;
    if (!SetPropW(dialog, marker.c_str(), reinterpret_cast<HANDLE>(1))) return false;
    auto record = std::make_unique<RecoveryRecord>();
    LASTINPUTINFO input{sizeof(input), 0};
    const bool valid = GetLastInputInfo(&input) &&
        !wcscpy_s(record->original_text, previous.c_str()) &&
        !wcscpy_s(record->attempted_text, attempted.c_str());
    if (valid) {
        record->pid = identity.process; record->thread = identity.thread; record->created = identity.process_created;
        record->dialog = identity.window; record->input_tick = input.dwTime;
        record->band = reinterpret_cast<uintptr_t>(band);
        record->toolbar = reinterpret_cast<uintptr_t>(toolbar);
        record->edit = reinterpret_cast<uintptr_t>(edit);
        InterlockedExchange(&record->stage, stage);
        RecoverInterrupted(marker, record.get(), true);
    }
    if (GetPropW(dialog, marker.c_str()) == reinterpret_cast<HANDLE>(1)) RemovePropW(dialog, marker.c_str());
    return valid;
}
#endif

int NativeDialogFolderLinkSupervisor(HANDLE parent, HANDLE helper, HANDLE heartbeat, HANDLE ready,
                                     HANDLE done, const wchar_t* marker) {
    TraceFolderLink(L"supervisor_start");
    const DWORD owner = parent ? GetProcessId(parent) : 0;
    const DWORD worker = helper ? GetProcessId(helper) : 0;
    const std::wstring name = marker ? marker : L"";
    GUID marker_id{};
    DWORD flags = 0;
    if (!owner || !worker || owner == worker || !GetHandleInformation(heartbeat, &flags) ||
        !GetHandleInformation(ready, &flags) || !GetHandleInformation(done, &flags) ||
        WaitForSingleObject(parent, 0) != WAIT_TIMEOUT || WaitForSingleObject(helper, 0) != WAIT_TIMEOUT ||
        !IsActualParent(owner) || !IsChildOf(helper, owner) || !SameExecutable(parent) ||
        !SameExecutable(helper) || !SameSecurity(owner) || !SameSecurity(worker) ||
        !name.starts_with(L"Pulse.FolderLink.") || FAILED(CLSIDFromString(name.c_str() + 17, &marker_id))) {
        TraceFolderLink(L"supervisor_identity_rejected");
        return 2;
    }
    auto* record = static_cast<RecoveryRecord*>(MapViewOfFile(heartbeat, FILE_MAP_ALL_ACCESS, 0, 0,
                                                             sizeof(RecoveryRecord)));
    if (!record) { TraceFolderLink(L"supervisor_mapping_failed", GetLastError()); return 2; }
    record->supervisor_pid = GetCurrentProcessId();
    record->supervisor_created = ProcessCreated(GetCurrentProcess());
    if (!record->supervisor_created || !SetEvent(ready)) {
        TraceFolderLink(L"supervisor_ready_failed", GetLastError()); UnmapViewOfFile(record); return 2;
    }
    TraceFolderLink(L"supervisor_ready");
    // Monitor both handles directly. After parent death its job terminates the
    // helper; if the kernel cannot confirm that within five seconds, retain
    // the marker and exit without touching the live window.
    HANDLE watched[] = {helper, parent};
    DWORD result = WaitForMultipleObjects(ARRAYSIZE(watched), watched, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 + 1) result = WaitForSingleObject(helper, 5000);
    TraceFolderLink(L"supervisor_helper_wait", result);
    if (result == WAIT_OBJECT_0 && InterlockedCompareExchange(&record->cleanup_claim, 1, 0) == 0) {
        HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE watchdog = finished ? CreateThread(nullptr, 0, RecoveryWatchdog, finished, 0, nullptr) : nullptr;
        if (!watchdog) { if (finished) CloseHandle(finished); UnmapViewOfFile(record); return 3; }
        RecoverInterrupted(name, record);
        CleanupMarker(name);
        InterlockedExchange(&record->cleanup_claim, 2);
        SetEvent(done);
        SetEvent(finished);
        WaitForSingleObject(watchdog, 100);
        CloseHandle(watchdog);
        CloseHandle(finished);
    }
    UnmapViewOfFile(record);
    TraceFolderLink(L"supervisor_exit", result);
    return result == WAIT_OBJECT_0 ? 0 : 3;
}

int NativeDialogFolderLinkHelper(HANDLE parent, HANDLE stop_event, HANDLE heartbeat, HANDLE ready, const wchar_t* marker) {
    TraceFolderLink(L"helper_start");
    DWORD flags = 0;
    const DWORD parent_pid = parent ? GetProcessId(parent) : 0;
    const std::wstring marker_value = marker ? marker : L"";
    GUID marker_id{};
    if (!parent_pid || parent_pid == GetCurrentProcessId() ||
        !GetHandleInformation(parent, &flags) || !GetHandleInformation(stop_event, &flags) ||
        !GetHandleInformation(heartbeat, &flags) || !GetHandleInformation(ready, &flags) ||
        WaitForSingleObject(parent, 0) != WAIT_TIMEOUT || WaitForSingleObject(stop_event, 0) != WAIT_TIMEOUT ||
        !IsActualParent(parent_pid) || !SameExecutable(parent) || !SameSecurity(parent_pid) ||
        !marker_value.starts_with(L"Pulse.FolderLink.") ||
        FAILED(CLSIDFromString(marker_value.c_str() + 17, &marker_id))) {
        TraceFolderLink(L"helper_identity_rejected"); return 2;
    }
    auto shared = std::make_shared<NativeDialogFolderLink::Shared>();
    auto* record = static_cast<RecoveryRecord*>(MapViewOfFile(heartbeat, FILE_MAP_WRITE, 0, 0, sizeof(RecoveryRecord)));
    if (!record) { TraceFolderLink(L"helper_mapping_failed", GetLastError()); return 2; }
    HANDLE ready_checks[] = {parent, stop_event, ready};
    const DWORD ready_result = WaitForMultipleObjects(ARRAYSIZE(ready_checks), ready_checks, FALSE, 5000);
    if (ready_result != WAIT_OBJECT_0 + 2) {
        TraceFolderLink(L"helper_ready_failed", ready_result);
        UnmapViewOfFile(record); return 2;
    }
    HANDLE supervisor = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, record->supervisor_pid);
    if (!supervisor || WaitForSingleObject(supervisor, 0) != WAIT_TIMEOUT ||
        !record->supervisor_created || ProcessCreated(supervisor) != record->supervisor_created ||
        !IsChildOf(supervisor, parent_pid) || !SameExecutable(supervisor) || !SameSecurity(record->supervisor_pid) ||
        WaitForSingleObject(parent, 0) != WAIT_TIMEOUT || WaitForSingleObject(stop_event, 0) != WAIT_TIMEOUT) {
        TraceFolderLink(L"helper_supervisor_rejected");
        CloseOwned(supervisor); UnmapViewOfFile(record); return 2;
    }
    volatile LONG64* deadline = &record->deadline;
    shared->owner_pid = parent_pid;
    shared->owner = parent;
    shared->stop_event = stop_event;
    shared->supervisor = supervisor;
    shared->deadline = deadline;
    shared->recovery = record;
    shared->marker = marker_value;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!finished) { TraceFolderLink(L"helper_finished_event_failed", GetLastError()); CloseOwned(supervisor); UnmapViewOfFile(record); return 3; }
    ParentWait wait{parent, stop_event, supervisor, finished, GetCurrentThreadId(), deadline};
    HANDLE waiter = CreateThread(nullptr, 0, WaitForParent, &wait, 0, nullptr);
    if (!waiter) { TraceFolderLink(L"helper_watchdog_failed", GetLastError()); CloseHandle(finished); CloseOwned(supervisor); UnmapViewOfFile(record); return 3; }
    TraceFolderLink(L"helper_ready");
    InterlockedExchange64(deadline, static_cast<LONG64>(GetTickCount64() + 5000));
    ThreadMain(new std::shared_ptr<NativeDialogFolderLink::Shared>(shared));
    SetEvent(finished);
    SetEvent(stop_event);
    WaitForSingleObject(waiter, INFINITE);
    CloseHandle(waiter);
    CloseHandle(finished);
    CloseOwned(supervisor);
    UnmapViewOfFile(record);
    TraceFolderLink(L"helper_exit");
    return 0;
}

NativeDialogFolderLink::NativeDialogFolderLink() {
    TraceFolderLink(L"parent_start");
    GUID marker_id{};
    wchar_t marker_text[64]{};
    if (FAILED(CoCreateGuid(&marker_id)) || !StringFromGUID2(marker_id, marker_text, ARRAYSIZE(marker_text))) {
        TraceFolderLink(L"parent_marker_failed"); return;
    }
    marker_ = std::wstring(L"Pulse.FolderLink.") + marker_text;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    stop_event_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    ready_event_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    heartbeat_mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(RecoveryRecord), nullptr);
    if (heartbeat_mapping_)
        recovery_view_ = MapViewOfFile(heartbeat_mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RecoveryRecord));
    if (recovery_view_) deadline_ = &static_cast<RecoveryRecord*>(recovery_view_)->deadline;
    HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, TRUE, GetCurrentProcessId());
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!stop_event_ || !ready_event_ || !heartbeat_mapping_ || !deadline_ || !parent || !job_ ||
        !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        TraceFolderLink(L"parent_resources_failed", GetLastError());
        CloseOwned(parent); CloseOwned(stop_event_); CloseOwned(job_); return;
    }
    wchar_t executable[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!size || size >= ARRAYSIZE(executable)) {
        TraceFolderLink(L"parent_executable_failed", GetLastError());
        CloseOwned(parent); CloseOwned(stop_event_); CloseOwned(job_); return;
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!bytes || !InitializeProcThreadAttributeList(attributes, 2, 0, &bytes)) {
        TraceFolderLink(L"parent_attributes_failed", GetLastError());
        CloseOwned(parent); CloseOwned(stop_event_); CloseOwned(job_); return;
    }
    HANDLE inherited[] = {parent, stop_event_, heartbeat_mapping_, ready_event_};
    const bool configured = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                       inherited, sizeof(inherited), nullptr, nullptr) &&
        UpdateProcThreadAttribute(attributes, 0, kJobListAttribute, &job_, sizeof(job_), nullptr, nullptr);
    PROCESS_INFORMATION process{};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    // The helper never reads stdin or writes to the parent's console.
    startup.StartupInfo.hStdInput = nullptr;
    startup.StartupInfo.hStdOutput = nullptr;
    startup.StartupInfo.hStdError = nullptr;
    startup.lpAttributeList = attributes;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --native-folder-link-helper " +
        std::to_wstring(reinterpret_cast<uintptr_t>(parent)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(stop_event_)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(heartbeat_mapping_)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(ready_event_)) + L" " + marker_;
    const bool created = configured && CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
        &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (!created) { TraceFolderLink(L"parent_helper_create_failed", GetLastError()); CloseOwned(parent); CloseOwned(stop_event_); CloseOwned(job_); return; }
    cleanup_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cleanup_event_ ||
        !StartSupervisor(parent, process.hProcess, heartbeat_mapping_, ready_event_, cleanup_event_, marker_, supervisor_) ||
        !StartFallback(process.hProcess, supervisor_, heartbeat_mapping_, cleanup_event_, marker_) ||
        ResumeThread(process.hThread) == DWORD(-1)) {
        TraceFolderLink(L"parent_supervisor_or_resume_failed", GetLastError());
        TerminateProcess(process.hProcess, 1); // child just created by this instance
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        CloseOwned(parent); CloseOwned(supervisor_);
        CloseOwned(cleanup_event_); CloseOwned(stop_event_); CloseOwned(job_); return;
    }
    CloseOwned(parent);
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    TraceFolderLink(L"parent_children_started");
}
NativeDialogFolderLink::~NativeDialogFolderLink() {
    if (stop_event_) SetEvent(stop_event_);
    CloseOwned(job_); // kill-on-close ends even a blocked helper
    if (cleanup_event_) WaitForSingleObject(cleanup_event_, 200);
    CloseOwned(cleanup_event_);
    CloseOwned(supervisor_);
    CloseOwned(process_);
    CloseOwned(stop_event_);
    CloseOwned(ready_event_);
    if (recovery_view_) UnmapViewOfFile(recovery_view_);
    CloseOwned(heartbeat_mapping_);
}
bool NativeDialogFolderLink::Stop() noexcept {
    if (!process_) return true;
    if (!stop_started_) {
        stop_started_ = GetTickCount64();
        if (!stop_started_) stop_started_ = 1;
        SetEvent(stop_event_);
    }
    if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
        if (GetTickCount64() - stop_started_ >= 1000 && job_) {
            TraceFolderLink(L"parent_helper_termination");
            TerminateJobObject(job_, 1);
        }
        return false;
    }
    if (supervisor_ && WaitForSingleObject(supervisor_, 0) != WAIT_OBJECT_0) return false;
    if (cleanup_event_ && WaitForSingleObject(cleanup_event_, 0) != WAIT_OBJECT_0) return false;
    CloseOwned(process_); CloseOwned(stop_event_); CloseOwned(job_);
    CloseOwned(cleanup_event_);
    CloseOwned(supervisor_); CloseOwned(ready_event_);
    return true;
}
bool NativeDialogFolderLink::IsStopping() noexcept {
    if (!stop_started_ && process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        if (!unexpected_exit_reported_) {
            DWORD code = 0;
            GetExitCodeProcess(process_, &code);
            TraceFolderLink(L"parent_helper_unexpected_exit", code);
            unexpected_exit_reported_ = true;
        }
        // Unexpected exit disables this instance until explicitly restarted;
        // this avoids a process respawn loop after repeated UIA failures.
        // The dedicated cleanup task watches the process handle directly.
        // Keep this failed instance inert until explicit stop/restart.
        return false;
    }
    if (!stop_started_ && deadline_) {
        const LONG64 deadline = InterlockedCompareExchange64(deadline_, 0, 0);
        if (deadline > 0 && GetTickCount64() >= static_cast<ULONGLONG>(deadline)) Stop();
    }
    return stop_started_ != 0;
}
} // namespace pulse::app
