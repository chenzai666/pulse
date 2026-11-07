#include "listary_adapter_endpoint.h"

#include "app_navigation.h"
#include "app_messages.h"
#include "app_network_locations.h"
#include "app_state.h"
#include "folder_source.h"
#include "listary_adapter_protocol.h"
#include "shell_window_sync.h"
#include "../common/current_user_security.h"
#include "../fs/bounded_enumeration.h"

#include <shlwapi.h>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pulse {
namespace {

constexpr ULONGLONG kDeadlineMs = 2000;

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

bool Integrity(HANDLE process, DWORD& level) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return false;
    Handle token(raw);
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &bytes);
    if (!bytes) return false;
    std::vector<BYTE> storage(bytes);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, storage.data(), bytes, &bytes)) return false;
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
    const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) return false;
    level = *GetSidSubAuthority(label->Label.Sid, count - 1);
    return true;
}

bool SameFile(const wchar_t* left, const wchar_t* right) {
    Handle a(CreateFileW(left, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    Handle b(CreateFileW(right, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    BY_HANDLE_FILE_INFORMATION x{}, y{};
    return a && b && GetFileInformationByHandle(a.value, &x) && GetFileInformationByHandle(b.value, &y) &&
        !(x.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(y.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
        x.nFileIndexLow == y.nFileIndexLow;
}

struct Validation {
    HWND target = nullptr, sender = nullptr;
    DWORD target_thread = 0, sender_thread = 0, sender_pid = 0;
    uint32_t tab = 0;
    uintptr_t token = 0;
    uintptr_t source_layout = 0, source_pane = 0, source_tab = 0;
    uint64_t source_revision = 0;
    ULONGLONG deadline = 0;
    std::wstring path, source_path;
    HANDLE sender_process = nullptr;
    std::atomic_bool cancelled = false, ready = false, valid = false;

    ~Validation() { if (sender_process) CloseHandle(sender_process); }

    bool Current() const { return !cancelled && GetTickCount64() < deadline; }

    bool SenderWindow() const {
        DWORD pid = 0;
        return GetWindowThreadProcessId(sender, &pid) == sender_thread && pid == sender_pid;
    }

    bool SenderAlive() const {
        DWORD exit_code = 0;
        return sender_process && GetExitCodeProcess(sender_process, &exit_code) && exit_code == STILL_ACTIVE;
    }

    bool Validate() {
        if (!Current() || !SenderWindow()) return false;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sender_pid));
        DWORD own_session = 0, sender_session = 0, own_integrity = 0, sender_integrity = 0;
        const auto sid = CurrentUserSidString();
        if (!process || sid.empty() || ProcessUserSidString(process.value) != sid ||
            !ProcessIdToSessionId(sender_pid, &sender_session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || own_session != sender_session ||
            !Integrity(process.value, sender_integrity) || !Integrity(GetCurrentProcess(), own_integrity) ||
            own_integrity != sender_integrity || !Current()) return false;
        wchar_t executable[32768]{}, associated[32768]{};
        DWORD chars = ARRAYSIZE(executable), associated_chars = ARRAYSIZE(associated);
        if (!QueryFullProcessImageNameW(process.value, 0, executable, &chars) ||
            _wcsicmp(PathFindFileNameW(executable), L"Listary.exe") != 0 ||
            AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE, L"listary", L"open",
                associated, &associated_chars) != S_OK ||
            _wcsicmp(PathFindFileNameW(associated), L"Listary.exe") != 0 ||
            !SameFile(executable, associated) || !Current()) return false;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        DWORD exit_code = 0;
        const bool valid_directory = Current() && attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) && SenderWindow() &&
            GetExitCodeProcess(process.value, &exit_code) && exit_code == STILL_ACTIVE;
        if (!valid_directory) return false;
        // Keep the authenticated process object alive through UI acceptance;
        // a recycled PID must not inherit an earlier request's identity.
        sender_process = process.value;
        process.value = nullptr;
        return true;
    }
};

struct Endpoint {
    const app::LayoutTab* layout = nullptr;
    const app::Pane* pane = nullptr;
    const app::Tab* folder = nullptr;
    uint64_t revision = 0;
    uint32_t generation = 0;
    std::wstring path;
    std::shared_ptr<Validation> pending;
};

// UI thread owns endpoints and generation; workers hold only immutable request
// identities plus atomics, never AppState or model pointers.
std::unordered_map<HWND, Endpoint> endpoints;
uint32_t next_generation = 0;
uintptr_t next_token = 0;

void Observe(AppState& state, Endpoint& endpoint) {
    const auto* tab = ActiveTab(state);
    const auto* layout = state.window_tabs.Active();
    if (layout == endpoint.layout && state.pane == endpoint.pane && tab == endpoint.folder &&
        (!tab || (tab->view_generation == endpoint.revision && tab->current_path == endpoint.path)) &&
        endpoint.generation) return;
    if (endpoint.pending) endpoint.pending->cancelled = true;
    endpoint.layout = layout;
    endpoint.pane = state.pane;
    endpoint.folder = tab;
    endpoint.revision = tab ? tab->view_generation : 0;
    endpoint.path = tab ? tab->current_path : L"";
    if (++next_generation == 0) ++next_generation;
    endpoint.generation = next_generation;
    SetPropW(state.hwnd, app::kListaryAdapterTab,
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(endpoint.generation)));
    PublishActiveFolderSource(state);
}

bool Available(const AppState& state) {
    return state.startupComplete && !state.core.exiting && IsWindowVisible(state.hwnd) &&
        IsWindowEnabled(state.hwnd) && !state.windowLifetimePins &&
        state.renameIndex < 0 && !state.addressEditing && !state.filterEditing &&
        !(state.hwndTagRenameEdit && IsWindowVisible(state.hwndTagRenameEdit)) &&
        !state.tabDragging && !state.tabDragPending && !state.paneDragPending &&
        !state.dragPending && !state.trayDragOut;
}

bool Target(const AppState& state, const Endpoint& endpoint, const Validation& request) {
    DWORD pid = 0;
    return state.hwnd == request.target &&
        GetWindowThreadProcessId(state.hwnd, &pid) == request.target_thread && pid == GetCurrentProcessId() &&
        GetPropW(state.hwnd, app::kListaryAdapterMarker) == reinterpret_cast<HANDLE>(1) &&
        GetPropW(state.hwnd, app::kFolderSourceMarker) == reinterpret_cast<HANDLE>(1) &&
        reinterpret_cast<uintptr_t>(GetPropW(state.hwnd, app::kListaryAdapterTab)) == request.tab &&
        endpoint.generation == request.tab &&
        reinterpret_cast<uintptr_t>(state.window_tabs.Active()) == request.source_layout &&
        reinterpret_cast<uintptr_t>(state.pane) == request.source_pane && state.pane &&
        reinterpret_cast<uintptr_t>(state.pane->ActiveTab()) == request.source_tab &&
        state.pane->ActiveTab() == endpoint.folder && endpoint.folder &&
        endpoint.folder->view_generation == request.source_revision &&
        endpoint.folder->current_path == request.source_path;
}

} // namespace

bool AttachListaryAdapter(AppState& state) {
    if (!state.hwnd || state.isolatedTest || state.shot.active || state.menushot) return false;
    if (endpoints.contains(state.hwnd)) return true;
    wchar_t name[128]{};
    if (!GetClassNameW(state.hwnd, name, ARRAYSIZE(name)) || wcscmp(name, L"PulseCustomMainWindow") != 0)
        return false;
    auto [entry, inserted] = endpoints.try_emplace(state.hwnd);
    if (!inserted) return false;
    Observe(state, entry->second);
    if (!GetPropW(state.hwnd, app::kListaryAdapterTab) ||
        !SetPropW(state.hwnd, app::kListaryAdapterMarker, reinterpret_cast<HANDLE>(1))) {
        DetachListaryAdapter(state);
        return false;
    }
    return true;
}

void DetachListaryAdapter(AppState& state) noexcept {
    RemovePropW(state.hwnd, app::kListaryAdapterMarker);
    RemovePropW(state.hwnd, app::kListaryAdapterTab);
    const auto entry = endpoints.find(state.hwnd);
    if (entry == endpoints.end()) return;
    if (entry->second.pending) entry->second.pending->cancelled = true;
    endpoints.erase(entry);
}

bool ListaryAdapterMessage(AppState& state, UINT message, WPARAM wparam,
                           LPARAM lparam, LRESULT& result) {
    const auto entry = endpoints.find(state.hwnd);
    if (entry == endpoints.end()) return false;
    auto& endpoint = entry->second;
    Observe(state, endpoint);
    if (message == WM_CLOSE || message == WM_DESTROY || message == WM_QUERYENDSESSION ||
        message == WM_ENDSESSION || (message == WM_SHOWWINDOW && !wparam)) {
        if (endpoint.pending) endpoint.pending->cancelled = true;
    }
    if (message == WM_LISTARY_ADAPTER_READY) {
        result = FALSE;
        const auto request = endpoint.pending;
        if (!request || request->token != wparam || !request->ready) return true;
        endpoint.pending.reset();
        if (request->valid && request->Current() && Target(state, endpoint, *request) &&
            request->SenderAlive() && request->SenderWindow() && Available(state) &&
            !IsSystemNetworkShortcut(state, request->path)) {
            NavigateTo(state, request->path);
            Observe(state, endpoint);
            result = TRUE;
        }
        return true;
    }
    if (message != WM_COPYDATA) return false;
    result = FALSE;
    const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
    if (!data || data->dwData != app::kListaryAdapterData) return false;
    const auto parsed = app::ParseListaryAdapterRequest(data->dwData, data->lpData, data->cbData);
    if (!parsed || !Available(state) || !ActiveTab(state) ||
        (endpoint.pending && !endpoint.pending->ready)) return true;
    auto request = std::make_shared<Validation>();
    request->target = state.hwnd;
    request->target_thread = GetCurrentThreadId();
    request->sender = reinterpret_cast<HWND>(wparam);
    request->sender_thread = GetWindowThreadProcessId(request->sender, &request->sender_pid);
    request->tab = parsed->tab;
    request->path = parsed->path;
    request->source_layout = reinterpret_cast<uintptr_t>(state.window_tabs.Active());
    request->source_pane = reinterpret_cast<uintptr_t>(state.pane);
    request->source_tab = reinterpret_cast<uintptr_t>(ActiveTab(state));
    request->source_revision = ActiveTab(state)->view_generation;
    request->source_path = ActiveTab(state)->current_path;
    request->deadline = GetTickCount64() + kDeadlineMs;
    if (!request->sender || !request->sender_thread || !request->sender_pid ||
        request->sender_pid == GetCurrentProcessId() || !Target(state, endpoint, *request)) return true;
    if (++next_token == 0) ++next_token;
    request->token = next_token;
    if (endpoint.pending) endpoint.pending->cancelled = true;
    endpoint.pending = request;
    state.worker.EnqueueIo([request] {
        try {
            request->valid = fs::RunBoundedEnumeration<bool>(request->path.starts_with(L"\\\\"),
                [request](const std::atomic_bool& cancelled) {
                    if (cancelled) return false;
                    DWORD previous = 0;
                    const BOOL changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous);
                    bool valid = false;
                    try { valid = request->Validate(); }
                    catch (...) { if (changed) SetThreadErrorMode(previous, nullptr); throw; }
                    if (changed) SetThreadErrorMode(previous, nullptr);
                    return valid;
                }, [request] { return !request->Current(); }, std::chrono::milliseconds(1800));
        }
        catch (...) { request->valid = false; }
        request->ready = true;
        if (request->Current()) PostMessageW(request->target, WM_LISTARY_ADAPTER_READY, request->token, 0);
    });
    // Acknowledges bounded asynchronous admission only; no file is opened or
    // confirmed, and the active tab is rechecked before ordinary navigation.
    result = TRUE;
    return true;
}

} // namespace pulse
