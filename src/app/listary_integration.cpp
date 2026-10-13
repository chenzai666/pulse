#include "listary_integration.h"

#include "app_hosted_edit.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "tray_reveal.h"
#include "../common/current_user_security.h"
#include "../ipc/deadline_pipe.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace pulse {
namespace {

constexpr uint32_t kMagic = 0x50554c50; // PULP, little endian
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxPathChars = 32767;
constexpr DWORD kRequestTimeoutMs = 2000;
enum class Operation : uint32_t {
    CurrentTab = 1, CurrentFolder = 2, OpenFolder = 3, SelectItem = 4,
    NewTabFolder = 5, NewTabSelectItem = 6
};
enum class Status : uint32_t {
    Ok = 0, NoFolder = 1, StaleTab = 2, Invalid = 3, Closing = 4, Busy = 5
};
struct RequestHeader {
    uint32_t magic = kMagic, version = kVersion, operation = 0, chars = 0;
    uint64_t window = 0, created = 0, tab = 0, deadline = 0;
};
struct ResponseHeader {
    uint32_t magic = kMagic, version = kVersion, status = 0, chars = 0;
    uint64_t tab = 0;
};
static_assert(sizeof(RequestHeader) == 48);
static_assert(sizeof(ResponseHeader) == 24);
static_assert(sizeof(wchar_t) == 2);

struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

bool Created(HANDLE process, uint64_t& created) {
    FILETIME start{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &start, &exit, &kernel, &user)) return false;
    created = (uint64_t{start.dwHighDateTime} << 32) | start.dwLowDateTime;
    return true;
}

std::wstring Hex(uint64_t value) {
    wchar_t text[17]{};
    swprintf_s(text, L"%016llX", static_cast<unsigned long long>(value));
    return text;
}

bool SameUserAndSession(HANDLE pipe) {
    ULONG pid = 0;
    DWORD own_session = 0, peer_session = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !pid ||
        !ProcessIdToSessionId(pid, &peer_session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
        peer_session != own_session) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    const auto own_sid = CurrentUserSidString();
    return process && !own_sid.empty() && ProcessUserSidString(process.value) == own_sid;
}

// Syntactic admission only. No filesystem/Shell calls run on the pipe worker
// or while the UI accepts a request. Enumeration stays in Pulse's workers.
bool NormalizeTarget(std::wstring input, std::wstring& output) {
    output.clear();
    if (input.empty() || input.size() > kMaxPathChars) return false;
    std::replace(input.begin(), input.end(), L'/', L'\\');
    if (_wcsnicmp(input.c_str(), L"\\\\?\\UNC\\", 8) == 0) input = L"\\\\" + input.substr(8);
    else if (input.starts_with(L"\\\\?\\")) input.erase(0, 4);
    const bool drive = input.size() >= 3 &&
        ((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z')) &&
        input[1] == L':' && input[2] == L'\\';
    const bool unc = input.starts_with(L"\\\\") && input.size() > 4 &&
        input[2] != L'.' && input[2] != L'?';
    if (!drive && !unc) return false;
    for (size_t i = 0; i < input.size(); ++i) {
        const wchar_t ch = input[i];
        if (ch < 32 || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' ||
            ch == L'*' || ch == L'?' || (ch == L':' && !(drive && i == 1))) return false;
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (++i >= input.size() || input[i] < 0xdc00 || input[i] > 0xdfff) return false;
        } else if (ch >= 0xdc00 && ch <= 0xdfff) return false;
    }
    if (unc) {
        const auto server_end = input.find(L'\\', 2);
        if (server_end == std::wstring::npos || server_end == 2 || server_end + 1 == input.size()) return false;
        const auto share_end = input.find(L'\\', server_end + 1);
        const auto share = input.substr(server_end + 1, share_end - server_end - 1);
        if (share.empty() || share == L"." || share == L"..") return false;
    }
    wchar_t absolute[32768]{};
    const DWORD chars = GetFullPathNameW(input.c_str(), ARRAYSIZE(absolute), absolute, nullptr);
    if (!chars || chars > kMaxPathChars) return false;
    output.assign(absolute, chars);
    while (output.size() > 3 && output.back() == L'\\') output.pop_back();
    return true;
}

struct Request {
    RequestHeader header;
    std::wstring path;
    Status status = Status::Invalid;
    uint64_t tab = 0;
    std::wstring folder;
    uintptr_t id = 0;
    Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool cancelled = false;
};

std::atomic_uintptr_t next_request_id{1};

struct Bridge {
    HWND window = nullptr;
    uint64_t created = 0;
    Handle pipe;
    Handle stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool stopping = false;
    std::thread worker;
    std::mutex pending_mutex;
    std::shared_ptr<Request> pending;
    // These fields are owned by the UI thread. Tokens never contain addresses.
    const app::LayoutTab* observed_layout = nullptr;
    const app::Pane* observed_pane = nullptr;
    uint64_t observed_generation = 0, tab_token = 0;
    std::wstring observed_path;

    ~Bridge() { Stop(); }

    void Stop() noexcept {
        stopping = true;
        if (stop) SetEvent(stop.value);
        if (pipe) CancelIoEx(pipe.value, nullptr);
        if (worker.joinable()) worker.join();
    }

    bool Connect() {
        OVERLAPPED operation{};
        Handle completed(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!completed || stopping) return false;
        operation.hEvent = completed.value;
        if (ConnectNamedPipe(pipe.value, &operation)) return true;
        const auto error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) return true;
        if (error != ERROR_IO_PENDING) return false;
        HANDLE events[]{stop.value, completed.value};
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe.value, &operation);
            DWORD transferred = 0;
            GetOverlappedResult(pipe.value, &operation, &transferred, TRUE);
            return false;
        }
        DWORD transferred = 0;
        return GetOverlappedResult(pipe.value, &operation, &transferred, FALSE) != FALSE;
    }

    bool Io(uint8_t* bytes, DWORD size, bool write, uint64_t deadline) {
        return ipc::DeadlinePipeIo(pipe.value, bytes, size, write, deadline,
                                  [this] { return stopping.load(); });
    }

    void Serve() {
        while (!stopping) {
            if (!Connect()) break;
            const auto connection_deadline = GetTickCount64() + kRequestTimeoutMs;
            auto request = std::make_shared<Request>();
            auto& header = request->header;
            bool valid = SameUserAndSession(pipe.value) && request->done &&
                Io(reinterpret_cast<uint8_t*>(&header), static_cast<DWORD>(sizeof(header)), false, connection_deadline);
            const auto now = GetTickCount64();
            valid = valid && header.magic == kMagic && header.version == kVersion &&
                header.window == reinterpret_cast<uintptr_t>(window) && header.created == created &&
                header.chars <= kMaxPathChars && header.operation >= 1 && header.operation <= 6 &&
                header.deadline > now && header.deadline - now <= kRequestTimeoutMs;
            if (valid && header.chars) {
                request->path.resize(header.chars);
                valid = Io(reinterpret_cast<uint8_t*>(request->path.data()),
                    header.chars * static_cast<DWORD>(sizeof(wchar_t)), false, header.deadline);
            }
            const auto op = static_cast<Operation>(header.operation);
            if (valid) {
                if (op == Operation::CurrentTab || op == Operation::CurrentFolder)
                    valid = header.chars == 0 && (op != Operation::CurrentTab || header.tab == 0);
                else {
                    std::wstring normalized;
                    valid = NormalizeTarget(std::move(request->path), normalized);
                    request->path = std::move(normalized);
                    if (valid && (op == Operation::SelectItem || op == Operation::NewTabSelectItem)) {
                        const auto slash = request->path.find_last_of(L'\\');
                        const auto parent = request->path.substr(0, slash + 1);
                        valid = slash != std::wstring::npos && slash + 1 < request->path.size() &&
                            NormalizeTarget(parent, normalized);
                    }
                }
            }
            if (valid && !stopping) {
                request->id = next_request_id.fetch_add(1);
                if (!request->id) request->id = next_request_id.fetch_add(1);
                {
                    std::lock_guard lock(pending_mutex);
                    pending = request;
                }
                if (PostMessageW(window, kListaryBridgeMessage, request->id, 0)) {
                    HANDLE events[]{stop.value, request->done.value};
                    const auto wait_started = GetTickCount64();
                    const auto remaining = header.deadline > wait_started
                        ? static_cast<DWORD>(header.deadline - wait_started) : 0;
                    valid = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, remaining) == WAIT_OBJECT_0 + 1;
                } else valid = false;
                request->cancelled = !valid;
                {
                    std::lock_guard lock(pending_mutex);
                    if (pending == request) pending.reset();
                }
            }
            if (valid && !stopping) {
                ResponseHeader response;
                response.status = static_cast<uint32_t>(request->status);
                response.tab = request->tab;
                response.chars = static_cast<uint32_t>(request->folder.size());
                bool delivered = Io(reinterpret_cast<uint8_t*>(&response),
                    static_cast<DWORD>(sizeof(response)), true, header.deadline);
                if (delivered && response.chars)
                    delivered = Io(reinterpret_cast<uint8_t*>(request->folder.data()),
                        response.chars * static_cast<DWORD>(sizeof(wchar_t)), true, header.deadline);
                // DisconnectNamedPipe discards unread bytes. A bounded client
                // receipt acknowledgement avoids racing the buffered response.
                uint8_t receipt = 0;
                if (delivered) Io(&receipt, 1, false, header.deadline);
            }
            // No FlushFileBuffers: a client that stops reading must not delay
            // window close. A response acknowledges UI acceptance, not listing.
            DisconnectNamedPipe(pipe.value);
        }
    }

    void ObserveTab(AppState& state) {
        const auto* layout = state.window_tabs.Active();
        const auto* pane = state.pane;
        const auto* tab = pane ? pane->ActiveTab() : nullptr;
        if (layout != observed_layout || pane != observed_pane ||
            (tab ? tab->view_generation : 0) != observed_generation ||
            (tab ? tab->current_path : std::wstring()) != observed_path) {
            observed_layout = layout;
            observed_pane = pane;
            observed_generation = tab ? tab->view_generation : 0;
            observed_path = tab ? tab->current_path : std::wstring();
            if (++tab_token == 0) ++tab_token;
        }
    }

    void Accept(AppState& state, Request& request) {
        if (stopping || request.cancelled || GetTickCount64() >= request.header.deadline) {
            request.status = Status::Closing;
            return;
        }
        ObserveTab(state);
        auto* tab = ActiveTab(state);
        if (!tab) { request.status = Status::NoFolder; return; }
        const auto op = static_cast<Operation>(request.header.operation);
        if (op != Operation::CurrentTab && request.header.tab && request.header.tab != tab_token) {
            request.status = Status::StaleTab;
            return;
        }
        request.tab = tab_token;
        if (op == Operation::CurrentTab || op == Operation::CurrentFolder) {
            request.status = NormalizeTarget(tab->current_path, request.folder) ? Status::Ok : Status::NoFolder;
            return;
        }
        // Do not silently discard a name the user is editing, or navigate a
        // window disabled by an owned modal dialog.
        if (state.renameIndex >= 0 || !IsWindowEnabled(window)) {
            request.status = Status::Busy;
            return;
        }
        if (state.addressEditing) HideAddressEditor(state, false);
        const bool select = op == Operation::SelectItem || op == Operation::NewTabSelectItem;
        const bool new_tab = op == Operation::NewTabFolder || op == Operation::NewTabSelectItem;
        std::wstring folder = request.path, leaf;
        if (select) {
            const auto slash = folder.find_last_of(L'\\');
            leaf = folder.substr(slash + 1);
            folder.resize(slash + 1);
        }
        if (new_tab) {
            if (TakeFreshStart(state)) StartFreshAt(state, folder);
            else OpenTabAt(state, folder);
        } else NavigateTo(state, folder);
        tab = ActiveTab(state);
        if (select && tab) SelectNameInTab(state, *tab, leaf);
        RevealWindow(state);
        InvalidateRect(state.hwnd, nullptr, FALSE);
        ObserveTab(state);
        request.tab = tab_token;
        request.status = Status::Ok;
    }
};

// The map is UI-thread-owned; each worker only owns its own Bridge and never
// accesses AppState. Closing a window cancels its pipe before freeing state.
std::unordered_map<HWND, std::shared_ptr<Bridge>> bridges;

} // namespace

bool AttachListaryWindow(AppState& state) {
    if (!state.hwnd || bridges.contains(state.hwnd)) return bridges.contains(state.hwnd);
    wchar_t window_class[128]{};
    if (!GetClassNameW(state.hwnd, window_class, ARRAYSIZE(window_class)) ||
        wcscmp(window_class, kListaryWindowClass) != 0) return false;
    try {
        DWORD session = 0;
        const auto sid = CurrentUserSidString();
        CurrentUserSecurityAttributes security;
        if (sid.empty() || !security || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return false;
        auto bridge = std::make_shared<Bridge>();
        bridge->window = state.hwnd;
        if (!bridge->stop || !Created(GetCurrentProcess(), bridge->created)) return false;
        const auto name = L"\\\\.\\pipe\\PulseCustom.Listary.v1." + sid + L"." +
            std::to_wstring(session) + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
            Hex(reinterpret_cast<uintptr_t>(state.hwnd)) + L"." + Hex(bridge->created);
        bridge->pipe.value = CreateNamedPipeW(name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, kRequestTimeoutMs, security.get());
        if (!bridge->pipe) return false;
        bridge->ObserveTab(state);
        bridges.emplace(state.hwnd, bridge);
        bridge->worker = std::thread([bridge] {
            try { bridge->Serve(); }
            catch (...) { bridge->stopping = true; SetEvent(bridge->stop.value); }
        });
        if (!SetPropW(state.hwnd, kListaryBridgeProperty, reinterpret_cast<HANDLE>(uintptr_t{1}))) {
            DetachListaryWindow(state);
            return false;
        }
        return true;
    } catch (...) {
        DetachListaryWindow(state);
        return false;
    }
}

void DetachListaryWindow(AppState& state) noexcept {
    if (!state.hwnd) return;
    RemovePropW(state.hwnd, kListaryBridgeProperty);
    const auto found = bridges.find(state.hwnd);
    if (found == bridges.end()) return;
    found->second->Stop();
    bridges.erase(found);
}

bool ListaryWindowMessage(AppState& state, UINT message, WPARAM wparam,
                         LPARAM, LRESULT& result) {
    if (message != kListaryBridgeMessage) return false;
    result = 0;
    const auto found = bridges.find(state.hwnd);
    if (found == bridges.end()) return true;
    const auto bridge = found->second;
    std::shared_ptr<Request> request;
    {
        std::lock_guard lock(bridge->pending_mutex);
        if (bridge->pending && bridge->pending->id == wparam) request = bridge->pending;
    }
    if (!request) return true;
    bridge->Accept(state, *request);
    SetEvent(request->done.value);
    return true;
}

} // namespace pulse
