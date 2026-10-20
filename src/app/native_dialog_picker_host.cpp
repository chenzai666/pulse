#include "native_dialog_takeover.h"
#include "native_dialog_wire.h"
#include "../common/localization.h"
#include "../common/windows_compat.h"

#include <limits>
#include <mutex>
#include <ole2.h>
#include <d2d1helper.h>
#include <tlhelp32.h>

namespace pulse::app {
namespace {
class OwnedHandle {
public:
    explicit OwnedHandle(HANDLE handle) : handle_(handle) {}
    ~OwnedHandle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    HANDLE get() const { return handle_; }
    void reset(HANDLE handle) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = handle;
    }
private:
    HANDLE handle_;
};

bool SourceMatches(const NativeFileDialog& source) {
    DWORD process = 0;
    wchar_t name[128]{};
    if (!source.window || !source.window_thread_id || !source.filename_window || !source.confirm_window ||
        !IsWindow(source.window) || GetAncestor(source.window, GA_ROOT) != source.window ||
        GetWindowThreadProcessId(source.window, &process) != source.window_thread_id || process != source.process_id ||
        !GetClassNameW(source.window, name, 128) || std::wstring_view(name) != L"#32770" ||
        !IsChild(source.window, source.filename_window) || GetDlgItem(source.window, IDOK) != source.confirm_window ||
        !GetClassNameW(source.filename_window, name, 128) || std::wstring_view(name) != L"Edit" ||
        !GetClassNameW(source.confirm_window, name, 128) || std::wstring_view(name) != L"Button") return false;
    OwnedHandle target(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, source.process_id));
    FILETIME created{}, exited{}, kernel{}, user{};
    return target.get() && GetProcessTimes(target.get(), &created, &exited, &kernel, &user) &&
        CompareFileTime(&created, &source.process_created) == 0;
}

bool ParentMatches(HANDLE parent, DWORD expected) {
    OwnedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W process{sizeof(process)};
    bool creator_matches = false;
    if (Process32FirstW(snapshot.get(), &process)) {
        do {
            if (process.th32ProcessID == GetCurrentProcessId()) {
                creator_matches = process.th32ParentProcessID == expected;
                break;
            }
        } while (Process32NextW(snapshot.get(), &process));
    }
    if (!creator_matches) return false;
    std::wstring current_path(32768, wchar_t{}), parent_path(32768, wchar_t{});
    const DWORD current_size = GetModuleFileNameW(nullptr, current_path.data(), static_cast<DWORD>(current_path.size()));
    DWORD parent_size = static_cast<DWORD>(parent_path.size());
    if (!current_size || current_size >= current_path.size() ||
        !QueryFullProcessImageNameW(parent, 0, parent_path.data(), &parent_size) ||
        !parent_size || parent_size >= parent_path.size()) return false;
    FILETIME parent_created{}, current_created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(parent, &parent_created, &exited, &kernel, &user) ||
        !GetProcessTimes(GetCurrentProcess(), &current_created, &exited, &kernel, &user) ||
        CompareFileTime(&parent_created, &current_created) > 0) return false;
    return CompareStringOrdinal(current_path.data(), static_cast<int>(current_size),
        parent_path.data(), static_cast<int>(parent_size), TRUE) == CSTR_EQUAL;
}

class PickerLifetime {
public:
    PickerLifetime(const NativeFileDialog& source, HANDLE parent, HANDLE cancelled)
        : source_(source), parent_(parent), cancelled_(cancelled),
          finished_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~PickerLifetime() {
        Close();
        if (worker_.get()) {
            SetEvent(finished_.get());
            WaitForSingleObject(worker_.get(), INFINITE);
        }
    }
    bool Start() {
        if (!finished_.get()) return false;
        worker_.reset(CreateThread(nullptr, 0, Watch, this, 0, nullptr));
        return worker_.get() != nullptr;
    }
    bool Closing() {
        std::lock_guard lock(mutex_);
        return closing_ || WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT ||
            WaitForSingleObject(cancelled_, 0) != WAIT_TIMEOUT;
    }
    bool BeginPicker(bool owner_enabled) {
        std::lock_guard lock(mutex_);
        if (!owner_enabled || closing_ || !SourceMatches(source_) || !IsWindowEnabled(source_.window) ||
            WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT || WaitForSingleObject(cancelled_, 0) != WAIT_TIMEOUT)
            return false;
        owner_started_ = true;
        owner_enabled_ = owner_enabled;
        return true;
    }
    bool Ready(HWND picker) {
        DWORD process = 0;
        if (!picker || GetWindowThreadProcessId(picker, &process) != GetCurrentThreadId() ||
            process != GetCurrentProcessId() || GetWindow(picker, GW_OWNER) != source_.window) return false;
        std::lock_guard lock(mutex_);
        picker_ = picker;
        if (closing_ || !SourceMatches(source_) || WaitForSingleObject(parent_, 0) != WAIT_TIMEOUT ||
            WaitForSingleObject(cancelled_, 0) != WAIT_TIMEOUT) {
            CloseLocked();
            return false;
        }
        return true;
    }
    void Close() {
        std::lock_guard lock(mutex_);
        CloseLocked();
    }
    void PickerFinished() {
        std::lock_guard lock(mutex_);
        RestoreOwnerLocked();
        picker_ = nullptr;
        owner_started_ = false;
    }
private:
    static DWORD WINAPI Watch(void* context) {
        auto& lifetime = *static_cast<PickerLifetime*>(context);
        const HANDLE handles[] = {lifetime.finished_.get(), lifetime.parent_, lifetime.cancelled_};
        const DWORD result = WaitForMultipleObjects(3, handles, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0) lifetime.Close();
        return 0;
    }
    void CloseLocked() {
        closing_ = true;
        DWORD process = 0;
        if (picker_ && GetWindowThreadProcessId(picker_, &process) && process == GetCurrentProcessId())
            PostMessageW(picker_, WM_CLOSE, 0, 0);
        RestoreOwnerLocked();
    }
    void RestoreOwnerLocked() {
        if (owner_started_ && owner_enabled_ && SourceMatches(source_) && !IsWindowEnabled(source_.window))
            EnableWindow(source_.window, TRUE);
    }
    NativeFileDialog source_;
    HANDLE parent_, cancelled_;
    OwnedHandle finished_;
    OwnedHandle worker_{nullptr};
    std::mutex mutex_;
    HWND picker_ = nullptr;
    bool closing_ = false, owner_started_ = false, owner_enabled_ = false;
};
}

bool PackNativeDialogPickerResult(bool picked, const ui::FilePickerResult& result, ipc::PayloadWriter& payload) {
    if (!payload.data().empty()) return false;
    if (!picked) {
        payload.PutU32(0); payload.PutU32(0); payload.PutU32(0);
        return true;
    }
    if (result.paths.empty() || result.paths.size() > 256 ||
        result.filter_index > (std::numeric_limits<uint32_t>::max)()) return false;
    size_t size = 12;
    for (const auto& path : result.paths) {
        if (path.empty() || !dialog_wire::TextValid(path)) return false;
        size += 4 + path.size() * sizeof(wchar_t);
        if (size > dialog_wire::kLimit) return false;
    }
    payload.PutU32(1); payload.PutU32(static_cast<uint32_t>(result.filter_index));
    payload.PutU32(static_cast<uint32_t>(result.paths.size()));
    for (const auto& path : result.paths) payload.PutString(path);
    return true;
}

int RunNativeDialogPicker(HANDLE input, HANDLE output, HANDLE cancelled) {
    DWORD flags = 0;
    if (!input || !output || !cancelled || input == output || input == cancelled || output == cancelled ||
        !GetHandleInformation(input, &flags) || !GetHandleInformation(output, &flags) ||
        !GetHandleInformation(cancelled, &flags) || GetFileType(input) != FILE_TYPE_PIPE ||
        GetFileType(output) != FILE_TYPE_PIPE || WaitForSingleObject(cancelled, 0) != WAIT_TIMEOUT) return 1;
    OwnedHandle inherited_input(input), inherited_output(output), inherited_cancelled(cancelled);
    if (!SetHandleInformation(input, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(cancelled, HANDLE_FLAG_INHERIT, 0)) return 1;
    dialog_wire::Header header;
    std::vector<uint8_t> payload;
    dialog_wire::RequestData request;
    if (!dialog_wire::Read(input, header, payload) || header.type != dialog_wire::Request ||
        !dialog_wire::UnpackRequest(payload, request) || !SourceMatches(request.source) ||
        request.parent == GetCurrentProcessId()) return 1;
    OwnedHandle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, request.parent));
    if (!parent.get() || !ParentMatches(parent.get(), request.parent)) return 1;
    PickerLifetime lifetime(request.source, parent.get(), cancelled);
    if (!lifetime.Start() || lifetime.Closing()) return 1;
    const HRESULT initialized = OleInitialize(nullptr);
    if (FAILED(initialized)) return 1;
    struct OleLifetime { ~OleLifetime() { OleUninitialize(); } } ole;
    compat::EnableDpiAwareness();
    // This process never runs the main window startup. Without the module,
    // resource strings stay empty (Cancel, column titles) and Pick() treats
    // the unset language as English.
    l10n::Initialize(GetModuleHandleW(nullptr), request.appearance.language.empty()
        ? std::wstring_view(L"system") : std::wstring_view(request.appearance.language));
    auto spec = request.appearance.style;
    spec.mode = request.source.kind == NativeDialogKind::Folder ? ui::PickerMode::Folder :
        request.source.kind == NativeDialogKind::Save ? ui::PickerMode::Save : ui::PickerMode::File;
    spec.title = request.source.title;
    spec.initial_path = request.source.initial_path;
    spec.filename = request.source.filename;
    spec.filters = request.source.filters;
    spec.filter_index = request.source.filter_index;
    spec.allow_multiselect = request.source.kind == NativeDialogKind::Open && request.source.allow_multiselect;
    const uint32_t accent = request.appearance.accent;
    const auto color = D2D1::ColorF(static_cast<float>((accent >> 16) & 255) / 255.0f,
        static_cast<float>((accent >> 8) & 255) / 255.0f, static_cast<float>(accent & 255) / 255.0f);
    if (lifetime.Closing() || !SourceMatches(request.source)) return 1;
    const bool owner_enabled = IsWindowEnabled(request.source.window) != FALSE;
    if (!lifetime.BeginPicker(owner_enabled)) return 1;
    spec.on_ready = [&](HWND picker) {
        if (!lifetime.Ready(picker)) return;
        ipc::PayloadWriter ready;
        ready.PutU64(reinterpret_cast<uintptr_t>(picker));
        ready.PutU32(GetCurrentProcessId()); ready.PutU32(GetCurrentThreadId());
        if (!dialog_wire::Write(output, dialog_wire::Ready, ready)) lifetime.Close();
    };
    ui::FilePickerResult result;
    bool picked = false;
    try {
        picked = ui::ShowFilePicker(request.source.window, spec, request.appearance.dark, color, result);
    } catch (...) {
        lifetime.Close();
        lifetime.PickerFinished();
        if (owner_enabled && SourceMatches(request.source) && !IsWindowEnabled(request.source.window))
            EnableWindow(request.source.window, TRUE);
        return 1;
    }
    lifetime.PickerFinished();
    ipc::PayloadWriter response;
    if (!PackNativeDialogPickerResult(picked && !lifetime.Closing() && SourceMatches(request.source), result, response)) return 1;
    return dialog_wire::Write(output, dialog_wire::Result, response) ? 0 : 1;
}
}
