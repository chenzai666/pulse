#include "native_dialog_takeover.h"
#include "native_dialog_wire.h"
#include "native_dialog_submission_policy.h"
#include "../common/command_line.h"
#include "../common/runtime_log.h"
#include <atomic>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <objbase.h>
#include <oleacc.h>
#include <thread>
#include <unordered_map>
#include <algorithm>

namespace pulse::app {
namespace {
using dialog_wire::kSessionProperty;
std::mutex property_mutex;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { Reset(); }
    void Reset(HANDLE next = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = next; }
};
bool ProcessCreated(DWORD process_id, FILETIME& created) {
    Handle process;
    process.Reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    FILETIME exited{}, kernel{}, user{};
    return process.value && GetProcessTimes(process.value, &created, &exited, &kernel, &user);
}
struct Session {
    HWND source = nullptr;
    DWORD source_pid = 0;
    DWORD source_thread = 0;
    FILETIME source_created{};
    ULONGLONG observed_time = GetTickCount64();
    const std::unique_ptr<unsigned char> nonce = std::make_unique<unsigned char>();
    std::wstring recovery_marker;
    std::atomic<bool> tag_claimed = false;
    std::atomic<bool> tag_released = false;
    std::atomic<HWND> source_filename = nullptr, source_confirm = nullptr;
    std::atomic<HWND> picker = nullptr;
    std::atomic<DWORD> picker_pid = 0;
    std::atomic<bool> done = false;
    std::atomic<bool> invalidated = false, presented = false;
    std::atomic<bool> submitting = false, failure_reported = false;
    std::atomic<bool> terminal_failure = false;
    std::atomic<bool> source_completed = false;
    std::atomic<ULONGLONG> retry_after = 0;
    Handle cancelled, worker;
    mutable std::mutex owner_mutex;
    std::mutex worker_mutex;
    bool owns_disable = false;
    bool concealed = false;
    bool added_layered = false;
    bool original_enabled = true;
    COLORREF original_color_key = 0;
    BYTE original_alpha = 255;
    DWORD original_layered_flags = LWA_ALPHA;
    bool SameWindowIdentity() const {
        DWORD pid = 0;
        FILETIME created{};
        return !invalidated && IsWindow(source) && GetWindowThreadProcessId(source, &pid) == source_thread && pid == source_pid &&
            ProcessCreated(pid, created) && CompareFileTime(&created, &source_created) == 0;
    }
    HANDLE TagValue() const { return reinterpret_cast<HANDLE>(nonce.get()); }
    bool SameWindow() const {
        return SameWindowIdentity() && (!tag_claimed || (GetPropW(source, kSessionProperty) == TagValue() &&
            GetPropW(source, recovery_marker.c_str()) == reinterpret_cast<HANDLE>(1)));
    }
    bool SameTrackedWindow() const {
        std::lock_guard lock(owner_mutex);
        // Only an intentional release carries retry accounting; mutations always require SameWindow().
        return SameWindow() || (!presented && tag_released && SameWindowIdentity());
    }
    bool BindIdentity() {
        std::lock_guard owner_lock(owner_mutex);
        if (tag_claimed) return SameWindow();
        if (!SameSource()) return false;
        std::lock_guard property_lock(property_mutex);
        GUID id{};
        wchar_t guid[40]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, guid, ARRAYSIZE(guid))) return false;
        recovery_marker = L"Pulse.NativeDialogTakeover.Recovery." + std::wstring(guid);
        if (GetPropW(source, kSessionProperty) ||
            !SetPropW(source, recovery_marker.c_str(), reinterpret_cast<HANDLE>(1))) return false;
        if (!SetPropW(source, kSessionProperty, TagValue())) {
            RemovePropW(source, recovery_marker.c_str());
            return false;
        }
        tag_claimed = true;
        return SameSource();
    }
    void RemoveOwnTag() {
        std::lock_guard property_lock(property_mutex);
        if (tag_claimed && GetPropW(source, kSessionProperty) == TagValue() &&
            GetPropW(source, recovery_marker.c_str()) == reinterpret_cast<HANDLE>(1) &&
            RemovePropW(source, kSessionProperty) == TagValue()) {
            RemovePropW(source, recovery_marker.c_str());
            tag_released = true;
        }
    }
    bool SameSource() const {
        const HWND filename = source_filename.load(), confirm = source_confirm.load();
        return SameWindow() && (!filename || IsChild(source, filename)) &&
            (!confirm || GetDlgItem(source, IDOK) == confirm);
    }
    bool IsConcealed() {
        std::lock_guard lock(owner_mutex);
        return concealed;
    }
    void Conceal(bool disable) {
        std::lock_guard lock(owner_mutex);
        if (!tag_claimed || !SameSource()) return;
        if (!concealed) {
            original_enabled = IsWindowEnabled(source) != FALSE;
            const LONG_PTR style = GetWindowLongPtrW(source, GWL_EXSTYLE);
            if (style & WS_EX_LAYERED) {
                if (!GetLayeredWindowAttributes(source, &original_color_key, &original_alpha,
                        &original_layered_flags)) return;
            } else {
                SetLastError(ERROR_SUCCESS);
                if (!SetWindowLongPtrW(source, GWL_EXSTYLE, style | WS_EX_LAYERED) &&
                    GetLastError() != ERROR_SUCCESS) return;
                added_layered = true;
            }
            // Alpha is set before the native dialog's first Show whenever its controls are ready.
            if (!SetLayeredWindowAttributes(source, 0, 0, LWA_ALPHA)) {
                if (added_layered) SetWindowLongPtrW(source, GWL_EXSTYLE, style);
                added_layered = false;
                return;
            }
            concealed = true;
            diagnostics::runtime::Event("native_dialog_concealed", {{"pid", source_pid},
                {"already_visible", IsWindowVisible(source) ? 1u : 0u}});
        }
        if (disable && IsWindowEnabled(source)) EnableWindow(source, FALSE);
        if (disable) owns_disable = true;
    }
    void Restore(bool release_tag = false) {
        std::lock_guard lock(owner_mutex);
        const bool same = SameWindow();
        if (same && owns_disable) EnableWindow(source, TRUE);
        if (same && concealed) {
            if (added_layered) {
                const LONG_PTR style = GetWindowLongPtrW(source, GWL_EXSTYLE);
                SetWindowLongPtrW(source, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
            } else {
                SetLayeredWindowAttributes(source, original_color_key, original_alpha, original_layered_flags);
            }
        }
        owns_disable = false;
        concealed = false;
        added_layered = false;
        if (release_tag || !presented) RemoveOwnTag();
    }
    void Cancel(bool restore = true) {
        if (cancelled.value) SetEvent(cancelled.value);
        if (restore) Restore();
        const HWND window = picker.load();
        DWORD pid = 0;
        if (window && GetWindowThreadProcessId(window, &pid) && pid == picker_pid.load()) PostMessageW(window, WM_CLOSE, 0, 0);
        std::lock_guard lock(worker_mutex);
        if (worker.value) CancelSynchronousIo(worker.value);
    }
};
struct Attempt { std::shared_ptr<Session> session; unsigned count = 0; };
using Attempts = std::unordered_map<HWND, Attempt>;
ULONGLONG EventTime(DWORD tick, ULONGLONG now) {
    constexpr ULONGLONG cycle = ULONGLONG{1} << 32;
    const ULONGLONG expanded = (now & ~(cycle - 1)) | tick;
    return expanded <= now ? expanded : expanded >= cycle ? expanded - cycle : 0;
}
bool DestroyApplies(const Session& session, HWND window, ULONGLONG event_time) {
    // Equal ticks are ambiguous; nonce checks protect mutations until identity is observable.
    return session.source == window && event_time > session.observed_time;
}
void EraseDestroyedAttempt(Attempts& seen, HWND window, ULONGLONG event_time) {
    const auto found = seen.find(window);
    if (found != seen.end() && DestroyApplies(*found->second.session, window, event_time)) seen.erase(found);
}
bool CanSubmit(const Session& session) { return session.tag_claimed && session.SameSource(); }
constexpr UINT kCandidateMessage = WM_APP + 173;
constexpr UINT kDestroyedMessage = WM_APP + 174;
constexpr UINT_PTR kSweepTimer = 1;
constexpr unsigned kMaximumAttempts = 3;
bool RetryAllowed(const Session& prior, unsigned attempts, ULONGLONG now) {
    return prior.done && !prior.presented && !prior.terminal_failure && attempts < kMaximumAttempts && now >= prior.retry_after;
}
thread_local NativeDialogTakeover::Shared* watcher = nullptr;
std::wstring Executable() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size); return path;
}
bool Candidate(HWND window, bool require_visible = true) {
    wchar_t klass[64]{};
    DWORD pid = 0;
    const HWND popup = window ? GetWindow(window, GW_ENABLEDPOPUP) : nullptr;
    if (popup && popup != window && GetWindow(popup, GW_OWNER) == window &&
        GetClassNameW(popup, klass, ARRAYSIZE(klass)) && wcscmp(klass, L"PulseFolderPickerWindow") == 0) return false;
    // Discovery also sees dialogs before their owner/controls are initialized.
    // The adapter verifies identity, permission and file-dialog structure before concealment.
    return window && GetAncestor(window, GA_ROOT) == window && (!require_visible || IsWindowVisible(window)) &&
        GetWindowThreadProcessId(window, &pid) && pid != GetCurrentProcessId() &&
        GetClassNameW(window, klass, ARRAYSIZE(klass)) && wcscmp(klass, L"#32770") == 0;
}
}

struct NativeDialogTakeover::Shared {
    std::atomic<bool> active = true;
    std::atomic<DWORD> thread = 0;
    std::mutex mutex;
    NativeDialogAppearance appearance;
    HDESK desktop = nullptr;
    std::wstring desktop_name;
    std::vector<std::shared_ptr<Session>> sessions;
    std::function<void()> failure;
};

namespace {
bool Cancelled(const std::shared_ptr<NativeDialogTakeover::Shared>& shared, const Session& session) {
    return !shared->active || WaitForSingleObject(session.cancelled.value, 0) == WAIT_OBJECT_0;
}
enum class CommitCompletion { Closed, NativeValidation, TimedOut, Cancelled, SubmissionFailed };
struct CommitPopup {
    uint64_t window = 0;
    uint32_t kind = 0, buttons = 0;
};
bool SourceClosed(const Session& session) {
    DWORD process = 0;
    return !IsWindow(session.source) ||
        GetWindowThreadProcessId(session.source, &process) != session.source_thread || process != session.source_pid ||
        !IsWindowVisible(session.source);
}
bool DismissNativeValidation(const std::shared_ptr<Session>& session) {
    if (!session->SameWindow()) return false;
    const HWND popup = GetWindow(session->source, GW_ENABLEDPOPUP);
    DWORD process = 0;
    wchar_t klass[64]{};
    if (!popup || popup == session->source || popup == session->picker.load() ||
        GetWindow(popup, GW_OWNER) != session->source || !IsWindowVisible(popup) ||
        GetWindowThreadProcessId(popup, &process) != session->source_thread || process != session->source_pid ||
        !GetClassNameW(popup, klass, ARRAYSIZE(klass)) ||
        (wcscmp(klass, L"#32770") != 0 && wcscmp(klass, L"TaskDialog") != 0)) return false;
    for (const int identifier : {IDCANCEL, IDNO}) {
        const HWND button = GetDlgItem(popup, identifier);
        const HWND parent = button ? GetParent(button) : nullptr;
        wchar_t name[32]{};
        DWORD button_process = 0, parent_process = 0;
        if (!button || !parent || (parent != popup && !IsChild(popup, parent)) ||
            !IsWindowVisible(button) || !IsWindowEnabled(button) ||
            !GetClassNameW(button, name, ARRAYSIZE(name)) || wcscmp(name, L"Button") != 0 ||
            GetWindowThreadProcessId(button, &button_process) != session->source_thread || button_process != session->source_pid ||
            GetWindowThreadProcessId(parent, &parent_process) != session->source_thread || parent_process != session->source_pid) continue;
        return PostMessageW(parent, WM_COMMAND, MAKEWPARAM(identifier, BN_CLICKED), reinterpret_cast<LPARAM>(button)) != FALSE;
    }
    // WM_CLOSE follows the host's normal cancellation semantics. Never invoke
    // OK/Yes/Save or infer acceptance from the popup's text.
    return PostMessageW(popup, WM_CLOSE, 0, 0) != FALSE;
}

bool CancelKnownSource(const std::shared_ptr<NativeDialogTakeover::Shared>& shared,
    const std::shared_ptr<Session>& session, const NativeFileDialog& source, std::wstring& reason) {
    if (!shared->active || !CanSubmit(*session)) return false;
    if (!IsWindowEnabled(source.window)) {
        if (DismissNativeValidation(session)) {
            const ULONGLONG deadline = GetTickCount64() + 500;
            while (session->SameWindow() && !IsWindowEnabled(source.window) && GetTickCount64() < deadline)
                Sleep(20);
        }
        if (!session->SameWindow()) return true;
        // Only our own modal disable may be released. An outstanding host popup
        // retains its modality and must be closed through its negative action.
        const HWND popup = GetWindow(source.window, GW_ENABLEDPOPUP);
        if (popup && popup != source.window && popup != session->picker.load() && IsWindowVisible(popup)) {
            reason = L"原程序仍在处理校验窗口。请关闭文件弹窗接管以恢复原程序并处理该校验。";
            return false;
        }
        EnableWindow(source.window, TRUE);
    }
    const auto cancelled = [&] { return !shared->active || !CanSubmit(*session); };
    if (!CancelNativeDialog(source, reason, cancelled)) return false;
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (shared->active && session->SameWindow() && IsWindowVisible(source.window) && GetTickCount64() < deadline)
        Sleep(20);
    if (!session->SameWindow() || !IsWindowVisible(source.window)) return true;
    reason = L"原程序尚未关闭文件选择窗口。请关闭文件弹窗接管以恢复原程序；本次没有再次发送保存。";
    return false;
}

CommitCompletion AwaitNativeCompletion(const std::shared_ptr<NativeDialogTakeover::Shared>& shared,
    const std::shared_ptr<Session>& session, CommitPopup& validation) {
    const ULONGLONG deadline = GetTickCount64() + 8000;
    HWND pending_popup = nullptr;
    DWORD pending_thread = 0;
    ULONGLONG popup_since = 0;
    do {
        if (Cancelled(shared, *session)) return CommitCompletion::Cancelled;
        if (SourceClosed(*session)) return CommitCompletion::Closed;
        if (!session->SameWindow()) return CommitCompletion::SubmissionFailed;
        const HWND popup = GetWindow(session->source, GW_ENABLEDPOPUP);
        DWORD pid = 0;
        const DWORD thread = popup ? GetWindowThreadProcessId(popup, &pid) : 0;
        if (popup && popup != session->source && popup != session->picker.load() &&
            IsWindowVisible(popup) && thread && pid == session->source_pid &&
            GetWindow(popup, GW_OWNER) == session->source && !IsWindowEnabled(session->source)) {
            const ULONGLONG now = GetTickCount64();
            if (popup != pending_popup || thread != pending_thread) {
                pending_popup = popup; pending_thread = thread; popup_since = now;
            }
            // Short-lived host progress windows are not user validation. Keep
            // the save concealed until the same modal popup remains stable.
            if (now - popup_since >= 200) {
                wchar_t klass[64]{};
                GetClassNameW(popup, klass, ARRAYSIZE(klass));
                validation.window = reinterpret_cast<uintptr_t>(popup);
                validation.kind = wcscmp(klass, L"#32770") == 0 ? 1u :
                    wcscmp(klass, L"TaskDialog") == 0 ? 2u : 3u;
                EnumChildWindows(popup, [](HWND child, LPARAM context) -> BOOL {
                    const int id = GetDlgCtrlID(child);
                    if (id >= IDOK && id <= IDCONTINUE && IsWindowVisible(child))
                        reinterpret_cast<CommitPopup*>(context)->buttons |= 1u << id;
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&validation));
                DismissNativeValidation(session);
                return CommitCompletion::NativeValidation;
            }
        } else {
            pending_popup = nullptr; pending_thread = 0; popup_since = 0;
        }
        WaitForSingleObject(session->cancelled.value, 20);
    } while (GetTickCount64() < deadline);
    return CommitCompletion::TimedOut;
}
void RunSession(const std::shared_ptr<NativeDialogTakeover::Shared>& shared, const std::shared_ptr<Session>& session) {
    if (shared->desktop && !SetThreadDesktop(shared->desktop)) { session->done = true; return; }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) { session->done = true; return; }
    struct ComGuard { ~ComGuard() { CoUninitialize(); } } com;
    struct RestoreGuard {
        std::shared_ptr<NativeDialogTakeover::Shared> shared;
        std::shared_ptr<Session> session;
        ~RestoreGuard() {
            if (shared->active && !session->source_completed && session->IsConcealed() && session->SameWindow()) {
                NativeFileDialog source;
                source.window = session->source;
                source.process_id = session->source_pid;
                source.window_thread_id = session->source_thread;
                source.process_created = session->source_created;
                source.filename_window = session->source_filename;
                source.confirm_window = session->source_confirm;
                std::wstring reason;
                if (!CancelKnownSource(shared, session, source, reason) && session->SameWindow()) {
                    session->terminal_failure = true;
                    session->Conceal(true);
                    if (!session->failure_reported.exchange(true) && shared->failure) {
                        try { shared->failure(); } catch (...) {}
                    }
                } else session->Restore();
            } else session->Restore();
            session->retry_after = GetTickCount64() + 500;
            diagnostics::runtime::Event("native_dialog_attempt_finished", {{"pid", session->source_pid},
                {"presented", session->presented ? 1u : 0u}, {"invalidated", session->invalidated ? 1u : 0u}});
            session->done = true;
        }
    } restore{shared, session};
    NativeFileDialog source;
    std::wstring reason;
    bool identified = false;
    const ULONGLONG identify_deadline = GetTickCount64() + 1500;
    do {
        if (!session->SameSource() || Cancelled(shared, *session)) return;
        if (!Candidate(session->source, false)) {
            // CREATE can precede the top-level window finishing initialization.
            WaitForSingleObject(session->cancelled.value, 10);
            continue;
        }
        if (!session->IsConcealed()) {
            NativeFileDialog early;
            if (CanConcealNativeFileDialog(session->source, early, reason)) {
                session->source_filename = early.filename_window;
                session->source_confirm = early.confirm_window;
                source = early;
                if (!session->BindIdentity()) return;
                session->Conceal(false);
            }
        }
        if (IsWindowVisible(session->source) && InspectNativeFileDialog(session->source, source, reason)) {
            identified = true;
            break;
        }
        WaitForSingleObject(session->cancelled.value, 10);
    } while (GetTickCount64() < identify_deadline);
    if (Cancelled(shared, *session)) return;
    std::wstring blocked_reason;
    if (!identified) {
        if (!session->IsConcealed() || !CanSubmit(*session)) return;
        blocked_reason = L"无法安全读取原程序的文件选择模式或文件类型，请取消本次操作。";
    }
    if (source.process_id != session->source_pid || source.window_thread_id != session->source_thread ||
        CompareFileTime(&source.process_created, &session->source_created) != 0 || !session->SameSource()) return;
    session->source_filename = source.filename_window; session->source_confirm = source.confirm_window;
    if (!session->BindIdentity()) return;
    session->Conceal(false);
    if (!session->IsConcealed()) return;
    dialog_wire::RequestData request;
    request.source = source; request.parent = GetCurrentProcessId();
    request.blocked_reason = blocked_reason;
    Handle parent_process;
    parent_process.Reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, request.parent));
    FILETIME parent_exited{}, parent_kernel{}, parent_user{};
    if (!parent_process.value || !GetProcessTimes(parent_process.value, &request.parent_created,
            &parent_exited, &parent_kernel, &parent_user)) return;
    request.parent_handle = parent_process.value;
    {
        std::lock_guard lock(session->owner_mutex);
        request.recovery_marker = session->recovery_marker;
        request.recovery_tag = reinterpret_cast<uintptr_t>(session->TagValue());
        request.original_layered = !session->added_layered;
        request.original_enabled = session->original_enabled;
        request.original_color_key = session->original_color_key;
        request.original_alpha = session->original_alpha;
        request.original_layered_flags = session->original_layered_flags;
    }
    { std::lock_guard lock(shared->mutex); request.appearance = shared->appearance; }
    ipc::PayloadWriter payload;
    // 单次请求留在匿名管道缓冲区内，不让外部 UI 提供者拖住主程序。
    if (!dialog_wire::PackRequest(request, payload) || payload.data().size() > 48 * 1024) return;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle input_read, input_write, output_read, output_write, process, primary;
    if (!CreatePipe(&input_read.value, &input_write.value, &security, 65536) ||
        !CreatePipe(&output_read.value, &output_write.value, &security, 65536) ||
        !SetHandleInformation(input_write.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(output_read.value, HANDLE_FLAG_INHERIT, 0)) return;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<uint8_t> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return;
    struct AttributesGuard { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributesGuard() { DeleteProcThreadAttributeList(value); } } attributes_guard{attributes};
    const std::array<HANDLE, 4> inherited{input_read.value, output_write.value, session->cancelled.value, parent_process.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr)) return;
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    if (!shared->desktop_name.empty()) startup.StartupInfo.lpDesktop = shared->desktop_name.data();
    const auto executable = Executable();
    if (executable.empty() || Cancelled(shared, *session)) return;
    std::wstring command = QuoteWindowsArgument(executable) + L" --native-dialog-picker " +
        std::to_wstring(reinterpret_cast<uintptr_t>(input_read.value)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(output_write.value)) + L" " +
        std::to_wstring(reinterpret_cast<uintptr_t>(session->cancelled.value));
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &info)) return;
    process.Reset(info.hProcess); primary.Reset(info.hThread); session->picker_pid = info.dwProcessId;
    { std::lock_guard lock(session->owner_mutex); session->owns_disable = true; }
    input_read.Reset(); output_write.Reset();
    if (!dialog_wire::Write(input_write.value, dialog_wire::Request, payload)) {
        SetEvent(session->cancelled.value); return;
    }
    const ULONGLONG started = GetTickCount64();
    std::vector<uint8_t> buffer;
    bool ready = false, completed = false;
    uint64_t last_transaction = 0;
    ULONGLONG completion_deadline = 0;
    NativeSubmissionPolicy submission_policy;
    while (!Cancelled(shared, *session) && (session->SameSource() || session->submitting)) {
        if (buffer.size() >= sizeof(dialog_wire::Header)) {
            dialog_wire::Header header{}; memcpy(&header, buffer.data(), sizeof(header));
            if (header.magic != dialog_wire::kMagic || !header.size || header.size > dialog_wire::kLimit) break;
            if (buffer.size() >= sizeof(header) + header.size) {
                ipc::PayloadReader in(buffer.data() + sizeof(header), header.size);
                bool consumed = false;
                if (header.type == dialog_wire::Ready && !ready) {
                    uint64_t raw = 0;
                    uint32_t claimed_pid = 0, claimed_thread = 0;
                    if (!in.GetU64(raw) || !raw || raw > (std::numeric_limits<uintptr_t>::max)() ||
                        !in.GetU32(claimed_pid) || !in.GetU32(claimed_thread) || in.remaining() ||
                        claimed_pid != info.dwProcessId || claimed_thread != info.dwThreadId) break;
                    const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(raw)); DWORD pid = 0;
                    const bool live = IsWindow(window) != FALSE;
                    if (live && (GetWindowThreadProcessId(window, &pid) != claimed_thread || pid != claimed_pid ||
                        GetWindow(window, GW_OWNER) != source.window)) break;
                    bool complete_result = false;
                    const size_t offset = sizeof(header) + header.size;
                    if (buffer.size() - offset >= sizeof(dialog_wire::Header)) {
                        dialog_wire::Header next{}; memcpy(&next, buffer.data() + offset, sizeof(next));
                        complete_result = next.magic == dialog_wire::kMagic && next.type == dialog_wire::Result &&
                            next.size >= 12 && next.size <= dialog_wire::kLimit &&
                            buffer.size() - offset >= sizeof(next) + next.size;
                    }
                    if ((live && IsWindowVisible(window)) || complete_result) {
                        std::lock_guard lock(session->owner_mutex);
                        if (Cancelled(shared, *session) || !session->SameSource()) break;
                        if (live) session->picker = window;
                        session->owns_disable = true;
                        session->presented = true;
                        ready = true; consumed = true;
                    }
                } else if (header.type == dialog_wire::Submit && ready && !completed) {
                    uint32_t selected = 0, index = 0, count = 0;
                    uint64_t transaction = 0;
                    std::vector<std::wstring> paths;
                    if (!in.GetU32(selected) || selected > 1 || !in.GetU32(index) || !in.GetU32(count) || count > 256 ||
                        (source.filters.empty() ? index != 0 : index >= source.filters.size()) ||
                        (count > 1 && (source.kind != NativeDialogKind::Open || !source.allow_multiselect))) break;
                    bool valid = true;
                    for (uint32_t i = 0; i < count; ++i) {
                        std::wstring path;
                        if (!in.GetString(path) || !dialog_wire::TextValid(path) || path.empty()) { valid = false; break; }
                        paths.push_back(std::move(path));
                    }
                    if (!valid || !in.GetU64(transaction) || !transaction || transaction <= last_transaction ||
                        in.remaining() || (selected ? paths.empty() : !paths.empty())) break;
                    last_transaction = transaction;
                    bool posted = false, submitted = false;
                    CommitCompletion completion = CommitCompletion::SubmissionFailed;
                    NativeSubmissionDecision decision;
                    reason.clear();
                    CommitPopup validation;
                    if (!submission_policy.Begin(selected == 0)) {
                        reason = L"上次请求的结果尚未确定，为避免重复操作，请取消本次文件选择。";
                    } else {
                        session->submitting = true;
                        if (!selected) {
                            submitted = SourceClosed(*session) ||
                                CancelKnownSource(shared, session, source, reason);
                            posted = submitted;
                            completion = submitted ? CommitCompletion::Closed : CommitCompletion::SubmissionFailed;
                        } else if (!blocked_reason.empty()) {
                            reason = blocked_reason;
                        } else if (CanSubmit(*session)) {
                            // The picker stays open. Enable only for the bounded
                            // adapter transaction; never restore opacity here.
                            EnableWindow(source.window, TRUE);
                            const auto cancelled = [&] { return Cancelled(shared, *session) || !CanSubmit(*session); };
                            submitted = CommitNativeDialogSelection(source, paths, index, reason, cancelled, &posted);
                            if (submitted || posted) completion = AwaitNativeCompletion(shared, session, validation);
                        } else {
                            reason = L"原程序的文件选择窗口已改变，请取消后重新发起。";
                        }
                        NativeSubmissionOutcome outcome = NativeSubmissionOutcome::Rejected;
                        if (completion == CommitCompletion::Closed) outcome = NativeSubmissionOutcome::Closed;
                        else if (completion == CommitCompletion::NativeValidation) outcome = NativeSubmissionOutcome::Validation;
                        else if (completion == CommitCompletion::TimedOut) outcome = NativeSubmissionOutcome::TimedOut;
                        else if (completion == CommitCompletion::Cancelled) outcome = NativeSubmissionOutcome::Cancelled;
                        else if (!blocked_reason.empty()) outcome = NativeSubmissionOutcome::Validation;
                        decision = submission_policy.Complete(outcome, posted);
                    }
                    if (completion == CommitCompletion::NativeValidation)
                        reason = L"原程序要求额外校验，本次未自动确认覆盖或保存。请取消后重新发起；如校验仍未关闭，请关闭文件弹窗接管恢复原程序。";
                    else if (completion == CommitCompletion::TimedOut)
                        reason = L"原程序未确认操作完成。为避免重复保存，已暂停提交；请取消后重新发起。";
                    if (!decision.accepted && reason.empty()) reason = L"原程序未接受本次操作，请取消后重新发起。";
                    diagnostics::runtime::Event("native_dialog_commit", {{"pid", source.process_id},
                        {"kind", static_cast<uint64_t>(source.kind)}, {"picked", selected},
                        {"submitted", submitted ? 1u : 0u}, {"confirmation_attempted", posted ? 1u : 0u},
                        {"completion", static_cast<uint64_t>(completion)}, {"retry_allowed", decision.retry_allowed ? 1u : 0u},
                        {"popup", validation.window}, {"popup_kind", validation.kind}, {"popup_buttons", validation.buttons}},
                        decision.accepted ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Warning);
                    ipc::PayloadWriter reply;
                    reply.PutU64(transaction);
                    reply.PutU32(decision.accepted ? 1u : 0u);
                    reply.PutU32(decision.retry_allowed ? 1u : 0u);
                    reply.PutString(decision.accepted ? std::wstring() : reason);
                    // Completion belongs to the native operation, even if the
                    // helper closes its reply pipe before receiving the ack.
                    if (decision.accepted) session->source_completed = true;
                    if (!dialog_wire::Write(input_write.value, dialog_wire::SubmitReply, reply)) break;
                    completed = decision.accepted;
                    if (completed) {
                        completion_deadline = GetTickCount64() + 3000;
                    }
                    else {
                        session->submitting = posted;
                        session->Conceal(true);
                    }
                    consumed = true;
                } else if (header.type == dialog_wire::Result && ready) {
                    // The helper may finish only after SubmitReply or explicit
                    // shutdown. This final frame never sends another native command.
                    break;
                } else break;
                if (consumed) {
                    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(sizeof(header) + header.size));
                    continue;
                }
            }
        }
        DWORD available = 0;
        if (!PeekNamedPipe(output_read.value, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available) {
            const size_t before = buffer.size();
            constexpr size_t maximum = dialog_wire::kLimit + 2 * sizeof(dialog_wire::Header) + 16;
            if (before > maximum || available > maximum - before) break;
            buffer.resize(before + available); DWORD read = 0;
            if (!ReadFile(output_read.value, buffer.data() + before, available, &read, nullptr) || !read) break;
            buffer.resize(before + read);
            continue;
        }
        if (!available && WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) break;
        if (ready && !completed) session->Conceal(true);
        if (!ready && GetTickCount64() - started > 15000) break;
        if (completed && GetTickCount64() >= completion_deadline) break;
        WaitForSingleObject(session->cancelled.value, 25);
    }
    // Only the guard chooses safe cancellation versus explicit shutdown recovery.
    // Never expose a still-live source merely because the helper finished or failed.
    session->Cancel(false);
}

struct Work { std::shared_ptr<NativeDialogTakeover::Shared> shared; std::shared_ptr<Session> session; };
DWORD WINAPI SessionThread(void* raw) {
    std::unique_ptr<Work> work(static_cast<Work*>(raw));
    try { RunSession(work->shared, work->session); }
    catch (...) {
        work->session->Cancel(false);
        if (work->shared->active && work->session->IsConcealed() && work->session->SameWindow()) {
            work->session->terminal_failure = true;
            if (!work->session->failure_reported.exchange(true) && work->shared->failure) {
                try { work->shared->failure(); } catch (...) {}
            }
        } else work->session->Restore();
        work->session->done = true;
    }
    return 0;
}
void CALLBACK OnWindow(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG child, DWORD, DWORD event_tick) {
    if (!watcher || !watcher->active || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    if (event == EVENT_OBJECT_DESTROY) {
        const ULONGLONG event_time = EventTime(event_tick, GetTickCount64());
        std::lock_guard lock(watcher->mutex);
        for (const auto& session : watcher->sessions) {
            if (DestroyApplies(*session, window, event_time)) {
                session->invalidated = true;
                if (!session->submitting) session->Cancel(false);
            }
        }
        PostThreadMessageW(watcher->thread, kDestroyedMessage, event_tick, reinterpret_cast<LPARAM>(window));
        return;
    }
    const HWND root = GetAncestor(window, GA_ROOT);
    if (Candidate(root, false))
        PostThreadMessageW(watcher->thread, kCandidateMessage, 0, reinterpret_cast<LPARAM>(root));
}
DWORD WINAPI WatchThread(void* raw) {
    std::unique_ptr<std::shared_ptr<NativeDialogTakeover::Shared>> holder(static_cast<std::shared_ptr<NativeDialogTakeover::Shared>*>(raw));
    auto shared = *holder; watcher = shared.get();
    if (shared->desktop && !SetThreadDesktop(shared->desktop)) { watcher = nullptr; return 0; }
    MSG message{}; PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    shared->thread = GetCurrentThreadId();
    const HWINEVENTHOOK created = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr, OnWindow, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    const HWINEVENTHOOK shown = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnWindow, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    const HWINEVENTHOOK focused = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWindow, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    const HWINEVENTHOOK destroyed = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, OnWindow, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    const UINT_PTR timer = SetTimer(nullptr, kSweepTimer, 500, nullptr);
    struct HookGuard {
        HWINEVENTHOOK created, shown, focused, destroyed; UINT_PTR timer;
        ~HookGuard() {
            if (created) UnhookWinEvent(created);
            if (shown) UnhookWinEvent(shown);
            if (focused) UnhookWinEvent(focused);
            if (destroyed) UnhookWinEvent(destroyed);
            if (timer) KillTimer(nullptr, timer);
            watcher = nullptr;
        }
    } hook_guard{created, shown, focused, destroyed, timer};
    const auto scan = [&] {
        EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND window, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<NativeDialogTakeover::Shared*>(param);
            if (Candidate(window)) PostThreadMessageW(state->thread, kCandidateMessage, 0, reinterpret_cast<LPARAM>(window));
            return TRUE;
        }, reinterpret_cast<LPARAM>(shared.get()));
    };
    try {
    Attempts seen;
    struct SeenGuard {
        Attempts& seen;
        ~SeenGuard() {
            for (const auto& entry : seen) {
                entry.second.session->Cancel(false);
                entry.second.session->Restore(true);
            }
        }
    } seen_guard{seen};
    scan();
    while (shared->active && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kCandidateMessage) {
            const HWND window = reinterpret_cast<HWND>(message.lParam); DWORD pid = 0;
            if (!Candidate(window, false) || !GetWindowThreadProcessId(window, &pid)) continue;
            unsigned attempts = 0;
            if (const auto previous = seen.find(window); previous != seen.end()) {
                const auto& prior = previous->second.session;
                if (prior->SameTrackedWindow()) {
                    attempts = previous->second.count;
                    if (!RetryAllowed(*prior, attempts, GetTickCount64())) continue;
                } else {
                    prior->invalidated = true;
                    prior->Cancel(false);
                }
            }
            auto session = std::make_shared<Session>(); session->source = window; session->source_pid = pid;
            session->source_thread = GetWindowThreadProcessId(window, nullptr);
            if (!ProcessCreated(pid, session->source_created)) continue;
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            session->cancelled.Reset(CreateEventW(&security, TRUE, FALSE, nullptr));
            if (!session->cancelled.value) continue;
            {
                std::lock_guard lock(shared->mutex);
                std::erase_if(shared->sessions, [](const auto& candidate) { return candidate->done.load(); });
                if (!shared->active || shared->sessions.size() >= 4) continue;
                shared->sessions.push_back(session);
                auto* work = new Work{shared, session};
                std::lock_guard worker_lock(session->worker_mutex);
                session->worker.Reset(CreateThread(nullptr, 0, SessionThread, work, 0, nullptr));
                if (!session->worker.value) { delete work; session->done = true; }
                else {
                    seen[window] = {session, attempts + 1};
                    diagnostics::runtime::Event("native_dialog_attempt", {{"pid", pid}, {"attempt", attempts + 1}});
                }
            }
        } else if (message.message == kDestroyedMessage) {
            EraseDestroyedAttempt(seen, reinterpret_cast<HWND>(message.lParam),
                EventTime(static_cast<DWORD>(message.wParam), GetTickCount64()));
        } else if (message.message == WM_TIMER) {
            scan();
            std::erase_if(seen, [](const auto& entry) { return !entry.second.session->SameTrackedWindow(); });
            std::lock_guard lock(shared->mutex);
            std::erase_if(shared->sessions, [](const auto& session) { return session->done.load(); });
        }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    }
    catch (...) { shared->active = false; }
    return 0;
}
}

bool NativeDialogTakeoverIdentityRegression(HWND window) {
    Session session;
    session.source = window;
    session.source_thread = GetWindowThreadProcessId(window, &session.source_pid);
    if (!ProcessCreated(session.source_pid, session.source_created) || !session.SameWindow()) return false;
    const FILETIME created = session.source_created;
    ++session.source_created.dwLowDateTime;
    if (session.SameWindow()) return false;
    session.source_created = created;
    ++session.source_thread;
    if (session.SameWindow()) return false;
    --session.source_thread;
    session.invalidated = true;
    if (session.SameWindow()) return false;
    session.done = true; session.retry_after = 100;
    if (RetryAllowed(session, 1, 99) || RetryAllowed(session, kMaximumAttempts, 100) ||
        !RetryAllowed(session, 1, 100)) return false;
    session.presented = true;
    return !RetryAllowed(session, 1, 100);
}

uint32_t NativeDialogTakeoverGenerationRegression() {
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Pulse takeover identity fixture",
        WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 0;
    struct WindowGuard {
        HWND window;
        ~WindowGuard() { RemovePropW(window, kSessionProperty); DestroyWindow(window); }
    } window_guard{window};
    uint32_t passed = 0;
    const LONG_PTR original_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const auto make_session = [&] {
        SetWindowLongPtrW(window, GWL_EXSTYLE, original_style);
        EnableWindow(window, TRUE);
        auto session = std::make_shared<Session>();
        session->source = window;
        session->source_thread = GetWindowThreadProcessId(window, &session->source_pid);
        if (!ProcessCreated(session->source_pid, session->source_created)) session->invalidated = true;
        return session;
    };
    const auto blocked_commit = [&](const Session& session) {
        NativeFileDialog dialog; dialog.window = window;
        std::wstring reason;
        bool checked = false;
        const bool submitted = CommitNativeDialogSelection(dialog, {}, 0, reason, [&] {
            checked = true;
            return !CanSubmit(session);
        });
        return checked && !submitted && !CanSubmit(session);
    };
    auto old = make_session(); old->observed_time = 100;
    auto current = make_session(); current->observed_time = 200;
    Attempts queued{{window, {current, 1}}};
    EraseDestroyedAttempt(queued, window, 150);
    const bool preserved = queued.contains(window) && !DestroyApplies(*current, window, 150) &&
        !DestroyApplies(*current, window, 200);
    queued[window] = {old, 1};
    EraseDestroyedAttempt(queued, window, 150);
    if (preserved && queued.empty() && EventTime(0xfffffffeu, 0x100000002ull) == 0xfffffffeull &&
        EventTime(1, 0x100000002ull) == 0x100000001ull) passed |= 1;

    unsigned char foreign = 0;
    const HANDLE foreign_tag = reinterpret_cast<HANDLE>(&foreign);
    auto occupied = make_session();
    if (SetPropW(window, kSessionProperty, foreign_tag)) {
        const bool rejected = !occupied->BindIdentity();
        occupied->Conceal(true); occupied->Restore(true);
        if (rejected && GetPropW(window, kSessionProperty) == foreign_tag &&
            GetWindowLongPtrW(window, GWL_EXSTYLE) == original_style && IsWindowEnabled(window)) passed |= 2;
        RemovePropW(window, kSessionProperty);
    }
    for (unsigned replaced = 0; replaced < 2; ++replaced) {
        auto session = make_session();
        if (!session->BindIdentity()) continue;
        session->Conceal(true);
        if (!session->IsConcealed()) { session->Restore(true); continue; }
        const LONG_PTR concealed_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        const bool changed = replaced ? SetPropW(window, kSessionProperty, foreign_tag) != FALSE
            : RemovePropW(window, kSessionProperty) == session->TagValue();
        const bool blocked = !session->SameWindow() && blocked_commit(*session);
        session->Conceal(true); session->Restore(true);
        BYTE alpha = 255; DWORD flags = 0;
        if (changed && blocked && !session->SameTrackedWindow() &&
            GetWindowLongPtrW(window, GWL_EXSTYLE) == concealed_style && !IsWindowEnabled(window) &&
            GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && alpha == 0 && (flags & LWA_ALPHA) &&
            GetPropW(window, kSessionProperty) == (replaced ? foreign_tag : nullptr)) passed |= replaced ? 8u : 4u;
        RemovePropW(window, kSessionProperty);
    }
    auto terminal = make_session();
    if (terminal->BindIdentity()) {
        terminal->presented = true;
        terminal->Conceal(true); terminal->Restore(); terminal->done = true;
        const bool retained = GetPropW(window, kSessionProperty) == terminal->TagValue() &&
            terminal->SameTrackedWindow() && !RetryAllowed(*terminal, 1, GetTickCount64()) &&
            GetWindowLongPtrW(window, GWL_EXSTYLE) == original_style && IsWindowEnabled(window);
        terminal->Restore(true);
        if (retained && !GetPropW(window, kSessionProperty) && !terminal->SameWindow() &&
            GetWindowLongPtrW(window, GWL_EXSTYLE) == original_style && IsWindowEnabled(window)) passed |= 16;
    }
    auto retry = make_session();
    if (retry->BindIdentity()) {
        retry->Conceal(true); retry->Restore();
        const bool finishing = retry->SameTrackedWindow() && !RetryAllowed(*retry, 1, GetTickCount64());
        retry->done = true;
        if (!GetPropW(window, kSessionProperty) && !retry->SameWindow() && retry->SameTrackedWindow() &&
            finishing && RetryAllowed(*retry, 1, GetTickCount64()) &&
            !RetryAllowed(*retry, kMaximumAttempts, GetTickCount64())) passed |= 32;
    }
    return passed;
}

NativeDialogTakeover::NativeDialogTakeover(NativeDialogAppearance appearance, HDESK desktop, std::wstring desktop_name,
    std::function<void()> failure) : shared_(std::make_shared<Shared>()) {
    shared_->failure = std::move(failure);
    shared_->appearance = std::move(appearance);
    shared_->desktop = desktop; shared_->desktop_name = std::move(desktop_name);
    auto* holder = new std::shared_ptr<Shared>(shared_);
    thread_ = CreateThread(nullptr, 0, WatchThread, holder, 0, nullptr);
    if (!thread_) { delete holder; shared_->active = false; }
}
NativeDialogTakeover::~NativeDialogTakeover() { Stop(); if (thread_) CloseHandle(thread_); }
void NativeDialogTakeover::UpdateAppearance(NativeDialogAppearance appearance) {
    std::lock_guard lock(shared_->mutex); shared_->appearance = std::move(appearance);
}
void NativeDialogTakeover::Stop() {
    shared_->active = false;
    std::vector<std::shared_ptr<Session>> sessions;
    { std::lock_guard lock(shared_->mutex); sessions = shared_->sessions; }
    for (const auto& session : sessions) session->Cancel(false);
    if (shared_->thread) PostThreadMessageW(shared_->thread, WM_QUIT, 0, 0);
    if (thread_) WaitForSingleObject(thread_, 1000);
}
}
