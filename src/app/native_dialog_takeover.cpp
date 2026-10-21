#include "native_dialog_takeover.h"
#include "native_dialog_wire.h"
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
    std::atomic<HWND> source_filename = nullptr, source_confirm = nullptr;
    std::atomic<HWND> picker = nullptr;
    std::atomic<DWORD> picker_pid = 0;
    std::atomic<bool> done = false;
    Handle cancelled, worker;
    std::mutex owner_mutex;
    std::mutex worker_mutex;
    bool owns_disable = false;
    bool concealed = false;
    bool added_layered = false;
    COLORREF original_color_key = 0;
    BYTE original_alpha = 255;
    DWORD original_layered_flags = LWA_ALPHA;
    bool SameWindow() const {
        DWORD pid = 0;
        FILETIME created{};
        return IsWindow(source) && GetWindowThreadProcessId(source, &pid) == source_thread && pid == source_pid &&
            ProcessCreated(pid, created) && CompareFileTime(&created, &source_created) == 0;
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
        if (!SameSource()) return;
        if (!concealed) {
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
    void Restore() {
        std::lock_guard lock(owner_mutex);
        if (!SameWindow()) { owns_disable = false; concealed = false; added_layered = false; return; }
        if (owns_disable) EnableWindow(source, TRUE);
        if (concealed) {
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
constexpr UINT kCandidateMessage = WM_APP + 173;
constexpr UINT_PTR kSweepTimer = 1;
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
    return window && (!require_visible || IsWindowVisible(window)) && IsWindowEnabled(window) && GetWindow(window, GW_OWNER) &&
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
};

namespace {
bool Cancelled(const std::shared_ptr<NativeDialogTakeover::Shared>& shared, const Session& session) {
    return !shared->active || WaitForSingleObject(session.cancelled.value, 0) == WAIT_OBJECT_0;
}
enum class CommitCompletion { Closed, NativeValidation, TimedOut, Cancelled };
struct CommitPopup {
    uint64_t window = 0;
    uint32_t kind = 0, buttons = 0;
};
CommitCompletion AwaitNativeCompletion(const std::shared_ptr<NativeDialogTakeover::Shared>& shared,
    const std::shared_ptr<Session>& session, CommitPopup& validation) {
    const ULONGLONG deadline = GetTickCount64() + 3000;
    HWND pending_popup = nullptr;
    DWORD pending_thread = 0;
    ULONGLONG popup_since = 0;
    do {
        if (Cancelled(shared, *session)) return CommitCompletion::Cancelled;
        if (!session->SameWindow() || !IsWindowVisible(session->source)) return CommitCompletion::Closed;
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
    struct RestoreGuard { std::shared_ptr<Session> session; ~RestoreGuard() { session->Restore(); session->done = true; } } restore{session};
    NativeFileDialog source;
    std::wstring reason;
    bool identified = false;
    const ULONGLONG identify_deadline = GetTickCount64() + 1500;
    do {
        if (!session->SameSource() || Cancelled(shared, *session)) return;
        if (!Candidate(session->source, false)) {
            // CREATE can precede the dialog becoming enabled. Keep waiting for
            // that initialization instead of consuming its later SHOW event.
            if (IsWindowVisible(session->source)) return;
            WaitForSingleObject(session->cancelled.value, 10);
            continue;
        }
        if (!session->IsConcealed()) {
            NativeFileDialog early;
            if (CanConcealNativeFileDialog(session->source, early, reason)) {
                session->Conceal(false);
            }
        }
        if (IsWindowVisible(session->source) && InspectNativeFileDialog(session->source, source, reason)) {
            identified = true;
            break;
        }
        WaitForSingleObject(session->cancelled.value, 10);
    } while (GetTickCount64() < identify_deadline);
    if (!identified || Cancelled(shared, *session)) return;
    if (source.process_id != session->source_pid || source.window_thread_id != session->source_thread ||
        CompareFileTime(&source.process_created, &session->source_created) != 0 || !session->SameSource()) return;
    session->source_filename = source.filename_window; session->source_confirm = source.confirm_window;
    session->Conceal(false);
    dialog_wire::RequestData request;
    request.source = source; request.parent = GetCurrentProcessId();
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
    const std::array<HANDLE, 3> inherited{input_read.value, output_write.value, session->cancelled.value};
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
    bool ready = false, received = false, picked = false;
    size_t filter_index = 0;
    std::vector<std::wstring> paths;
    while (!Cancelled(shared, *session) && session->SameSource()) {
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
                        ready = true; consumed = true;
                    }
                } else if (header.type == dialog_wire::Result && ready) {
                    uint32_t selected = 0, index = 0, count = 0;
                    if (!in.GetU32(selected) || selected > 1 || !in.GetU32(index) || !in.GetU32(count) || count > 256 ||
                        (source.filters.empty() ? index != 0 : index >= source.filters.size()) ||
                        (count > 1 && (source.kind != NativeDialogKind::Open || !source.allow_multiselect))) break;
                    bool valid = true;
                    for (uint32_t i = 0; i < count; ++i) {
                        std::wstring path;
                        if (!in.GetString(path) || !dialog_wire::TextValid(path) || path.empty()) { valid = false; break; }
                        paths.push_back(std::move(path));
                    }
                    if (!valid || in.remaining() || (selected ? paths.empty() : !paths.empty())) break;
                    picked = selected != 0; filter_index = index; received = true; break;
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
        if (ready) session->Conceal(true);
        if (!ready && GetTickCount64() - started > 15000) break;
        WaitForSingleObject(session->cancelled.value, 25);
    }
    if (received && !Cancelled(shared, *session) && session->SameSource()) {
        // Inspect rejects disabled dialogs. The polling conceal can disable the source
        // after the helper enabled it, so restore that state before validating the handoff.
        EnableWindow(session->source, TRUE);
        if (NativeDialogStillMatches(source)) {
            const auto cancelled = [&] { return Cancelled(shared, *session); };
            const bool submitted = picked ? CommitNativeDialogSelection(source, paths, filter_index, reason, cancelled)
                : !cancelled() && CancelNativeDialog(source, reason, cancelled);
            // Posting the button notification precedes the native modal loop handling it.
            // Keep the source concealed through that handoff; never send a second confirmation.
            CommitPopup validation;
            const auto completion = AwaitNativeCompletion(shared, session, validation);
            diagnostics::runtime::Event("native_dialog_commit", {{"pid", source.process_id},
                {"kind", static_cast<uint64_t>(source.kind)}, {"picked", picked ? 1u : 0u},
                {"submitted", submitted ? 1u : 0u}, {"completion", static_cast<uint64_t>(completion)},
                {"popup", validation.window}, {"popup_kind", validation.kind}, {"popup_buttons", validation.buttons}},
                completion == CommitCompletion::TimedOut ? diagnostics::runtime::Level::Warning : diagnostics::runtime::Level::Info);
        } else {
            diagnostics::runtime::Event("native_dialog_handoff_rejected", {{"pid", source.process_id}},
                diagnostics::runtime::Level::Warning);
        }
    }
    session->Restore();
    // 关闭接管仅关闭我们自己的 picker；原生对话框不提交、不取消。
    session->Cancel();
}

struct Work { std::shared_ptr<NativeDialogTakeover::Shared> shared; std::shared_ptr<Session> session; };
DWORD WINAPI SessionThread(void* raw) {
    std::unique_ptr<Work> work(static_cast<Work*>(raw));
    try { RunSession(work->shared, work->session); }
    catch (...) { work->session->Cancel(); work->session->done = true; }
    return 0;
}
void CALLBACK OnWindow(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
    if (watcher && watcher->active && object == OBJID_WINDOW && child == CHILDID_SELF && Candidate(window, false))
        PostThreadMessageW(watcher->thread, kCandidateMessage, 0, reinterpret_cast<LPARAM>(window));
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
    const UINT_PTR timer = SetTimer(nullptr, kSweepTimer, 500, nullptr);
    struct HookGuard {
        HWINEVENTHOOK created, shown, focused; UINT_PTR timer;
        ~HookGuard() {
            if (created) UnhookWinEvent(created);
            if (shown) UnhookWinEvent(shown);
            if (focused) UnhookWinEvent(focused);
            if (timer) KillTimer(nullptr, timer);
            watcher = nullptr;
        }
    } hook_guard{created, shown, focused, timer};
    const auto scan = [&] {
        EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND window, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<NativeDialogTakeover::Shared*>(param);
            if (Candidate(window)) PostThreadMessageW(state->thread, kCandidateMessage, 0, reinterpret_cast<LPARAM>(window));
            return TRUE;
        }, reinterpret_cast<LPARAM>(shared.get()));
    };
    try {
    std::unordered_map<HWND, DWORD> seen;
    scan();
    while (shared->active && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == kCandidateMessage) {
            const HWND window = reinterpret_cast<HWND>(message.lParam); DWORD pid = 0;
            if (!Candidate(window, false) || !GetWindowThreadProcessId(window, &pid) || seen.contains(window)) continue;
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
                else seen[window] = pid;
            }
        } else if (message.message == WM_TIMER) {
            scan();
            std::erase_if(seen, [](const auto& entry) { DWORD pid = 0; return !GetWindowThreadProcessId(entry.first, &pid) || pid != entry.second; });
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

NativeDialogTakeover::NativeDialogTakeover(NativeDialogAppearance appearance, HDESK desktop, std::wstring desktop_name) : shared_(std::make_shared<Shared>()) {
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
