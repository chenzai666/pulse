#pragma once

#include "folder_source.h"
#include "../common/edition.h"

#include <mutex>
#include <utility>
#include <vector>

namespace pulse::app {

// Reads only the parent's published Edit; no Shell, filesystem or Listary calls.
class NativeFolderFollower {
public:
    using ForegroundReader = HWND (*)(void*);

    NativeFolderFollower(HANDLE parent, DWORD parent_id, ForegroundReader foreground = nullptr, void* context = nullptr)
        : parent_id_(parent_id), foreground_(foreground), context_(context) {
        if (!DuplicateHandle(GetCurrentProcess(), parent, GetCurrentProcess(), &parent_, 0, FALSE,
                DUPLICATE_SAME_ACCESS)) return;
        FILETIME exited{}, kernel{}, user{};
        identity_valid_ = GetProcessId(parent_) == parent_id_ &&
            GetProcessTimes(parent_, &created_, &exited, &kernel, &user);
    }
    ~NativeFolderFollower() {
        if (stopped_) SetEvent(stopped_);
        if (worker_) { WaitForSingleObject(worker_, INFINITE); CloseHandle(worker_); }
        if (stopped_) CloseHandle(stopped_);
        if (parent_) CloseHandle(parent_);
    }
    NativeFolderFollower(const NativeFolderFollower&) = delete;
    NativeFolderFollower& operator=(const NativeFolderFollower&) = delete;

    bool Start() {
        if (worker_ || !identity_valid_) return false;
        if (!stopped_) stopped_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopped_) return false;
        worker_ = CreateThread(nullptr, 0, Watch, this, 0, nullptr);
        return worker_ != nullptr;
    }
    void SetPicker(HWND picker) {
        std::lock_guard lock(mutex_);
        picker_ = picker;
    }
    // UI polling never sends messages or waits for the worker's lock.
    std::wstring Poll() {
        const HWND foreground = Foreground();
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !picker_ || foreground != picker_) return {};
        source_active_ = false;
        if (pending_.empty() || !pending_ready_) return {};
        if (!Matches(source_)) { ClearLocked(); return {}; }
        return std::exchange(pending_, {});
    }

private:
    HWND Foreground() const { return foreground_ ? foreground_(context_) : GetForegroundWindow(); }
    bool Matches(HWND window) const {
        DWORD process = 0;
        wchar_t name[128]{};
        FILETIME created{}, exited{}, kernel{}, user{};
        return identity_valid_ && WaitForSingleObject(parent_, 0) == WAIT_TIMEOUT &&
            GetProcessTimes(parent_, &created, &exited, &kernel, &user) &&
            CompareFileTime(&created, &created_) == 0 && window && IsWindow(window) &&
            GetAncestor(window, GA_ROOT) == window && GetWindowThreadProcessId(window, &process) &&
            process == parent_id_ && GetClassNameW(window, name, ARRAYSIZE(name)) &&
            std::wstring_view(name) == edition::kWindowClass &&
            GetPropW(window, kFolderSourceMarker) == reinterpret_cast<HANDLE>(1);
    }
    bool Read(HWND window, std::wstring& folder) const {
        if (!Matches(window)) return false;
        const HWND control = GetDlgItem(window, kFolderSourceControl);
        wchar_t name[32]{};
        DWORD source_process = 0, control_process = 0;
        const DWORD source_thread = GetWindowThreadProcessId(window, &source_process);
        if (!control || GetParent(control) != window || !GetClassNameW(control, name, ARRAYSIZE(name)) ||
            std::wstring_view(name) != L"Edit" || !(GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY) ||
            !source_thread || source_process != parent_id_ ||
            GetWindowThreadProcessId(control, &control_process) != source_thread || control_process != parent_id_)
            return false;
        std::vector<wchar_t> text(32768, L'\0');
        DWORD_PTR copied = 0;
        if (!SendMessageTimeoutW(control, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()),
                SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 75, &copied) || copied >= text.size() - 1 ||
            !Matches(window) || GetDlgItem(window, kFolderSourceControl) != control ||
            GetWindowThreadProcessId(window, &source_process) != source_thread || source_process != parent_id_ ||
            GetParent(control) != window || GetWindowThreadProcessId(control, &control_process) != source_thread ||
            control_process != parent_id_ || !GetClassNameW(control, name, ARRAYSIZE(name)) ||
            std::wstring_view(name) != L"Edit" || !(GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY))
            return false;
        folder.assign(text.data(), static_cast<size_t>(copied));
        if (!IsFolderSourcePath(folder)) folder.clear();
        return true;
    }
    static bool SamePath(std::wstring_view a, std::wstring_view b) {
        while (a.size() > 3 && a.back() == L'\\') a.remove_suffix(1);
        while (b.size() > 3 && b.back() == L'\\') b.remove_suffix(1);
        return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE)
            == CSTR_EQUAL;
    }
    void ClearLocked() {
        source_ = nullptr; source_active_ = false; pending_ready_ = false;
        previous_.clear(); pending_.clear();
    }
    void Sample(HWND activated = nullptr) {
        const HWND foreground = Foreground();
        HWND picker = nullptr, source = nullptr;
        {
            std::lock_guard lock(mutex_);
            picker = picker_;
            source = source_;
        }
        // On return, read the authenticated source once more on this worker.
        // Its final tab/navigation publication can occur after our last
        // foreground sample. Poll must not consume that earlier snapshot.
        if (activated && foreground != activated && foreground != picker) return;
        const bool returning = !activated && picker && foreground == picker && source;
        const HWND observed = activated ? activated : returning ? source : foreground;
        std::wstring path;
        const bool read = Read(observed, path);
        const HWND current = Foreground();
        if (current != foreground && current != picker) return;
        std::lock_guard lock(mutex_);
        if (returning && (picker_ != picker || source_ != source)) return;
        if (read) {
            if (path.empty()) { ClearLocked(); return; }
            if ((!returning && !source_active_) || source_ != observed || !SamePath(previous_, path))
                pending_ = path;
            pending_ready_ = returning && current == picker;
            source_active_ = !returning && current == observed;
            source_ = observed;
            previous_ = std::move(path);
        } else {
            source_active_ = false;
            if (returning) pending_ready_ = false;
            if (foreground == source_ || (source_ && !Matches(source_))) ClearLocked();
        }
    }
    static void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child,
                                      DWORD, DWORD event_time) {
        // Hooks are dispatched only by this follower's worker message pump.
        // Keep the event's source even when a quick switch has already ended.
        if (!watching_ || object != OBJID_WINDOW || child != CHILDID_SELF ||
            WaitForSingleObject(watching_->stopped_, 0) != WAIT_TIMEOUT ||
            GetTickCount() - event_time > 500 ||
            (watching_->have_foreground_event_ &&
                static_cast<LONG>(event_time - watching_->last_foreground_event_) < 0) ||
            !watching_->Matches(window)) return;
        watching_->have_foreground_event_ = true;
        watching_->last_foreground_event_ = event_time;
        watching_->Sample(window);
    }
    static DWORD WINAPI Watch(void* context) {
        auto& self = *static_cast<NativeFolderFollower*>(context);
        watching_ = &self;
        const HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, OnForeground, self.parent_id_, 0, WINEVENT_OUTOFCONTEXT);
        const HANDLE handles[] = {self.stopped_, self.parent_};
        for (;;) {
            const DWORD result = MsgWaitForMultipleObjects(2, handles, FALSE, 125, QS_ALLINPUT);
            if (result != WAIT_TIMEOUT && result != WAIT_OBJECT_0 + 2) break;
            if (result == WAIT_OBJECT_0 + 2) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                    if (WaitForMultipleObjects(2, handles, FALSE, 0) != WAIT_TIMEOUT) break;
                }
            }
            if (WaitForMultipleObjects(2, handles, FALSE, 0) != WAIT_TIMEOUT) break;
            self.Sample();
        }
        if (hook) UnhookWinEvent(hook);
        watching_ = nullptr;
        return 0;
    }
    inline static thread_local NativeFolderFollower* watching_ = nullptr;
    HANDLE parent_ = nullptr, stopped_ = nullptr, worker_ = nullptr;
    DWORD parent_id_;
    FILETIME created_{};
    bool identity_valid_ = false;
    bool have_foreground_event_ = false;
    DWORD last_foreground_event_ = 0;
    bool source_active_ = false;
    bool pending_ready_ = false;
    ForegroundReader foreground_;
    void* context_;
    std::mutex mutex_;
    HWND picker_ = nullptr, source_ = nullptr;
    std::wstring previous_, pending_;
};

} // namespace pulse::app
