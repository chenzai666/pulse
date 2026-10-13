#include "quicklook_integration.h"
#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulse::app {
namespace {

constexpr DWORD kStartupTimeout = 30000;
constexpr DWORD kCommandTimeout = 15000;
constexpr size_t kMaxProtocolLine = 8192;
std::atomic<UINT_PTR> next_token{1};
std::atomic<uint64_t> next_pipe{1};

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void Reset(HANDLE handle = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = handle;
    }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};

bool OwnerValid(HWND owner) {
    DWORD pid = 0;
    return owner && IsWindow(owner) && GetWindowThreadProcessId(owner, &pid) &&
        pid == GetCurrentProcessId();
}

bool ValidPathShape(std::wstring_view path) {
    if (path.empty() || path.size() > 32767) return false;
    const bool extended_unc = path.starts_with(L"\\\\?\\UNC\\");
    if (extended_unc) path.remove_prefix(8);
    else if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    const bool drive = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = extended_unc || (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\.\\"));
    if (!drive && !unc) return false;
    if (unc) {
        const size_t server_start = extended_unc ? 0 : 2;
        const auto server_end = path.find(L'\\', server_start);
        if (server_end == std::wstring_view::npos || server_end == server_start || server_end + 1 >= path.size())
            return false;
        const auto share_end = path.find(L'\\', server_end + 1);
        if (share_end == server_end + 1) return false;
    }
    for (size_t i = 0; i < path.size(); ++i) {
        const wchar_t ch = path[i];
        if (ch < L' ' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' ||
            ch == L'*' || ch == L'?' || (ch == L':' && (!drive || i != 1))) return false;
    }
    return true;
}

std::string Utf8(const std::wstring& path) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
        static_cast<int>(path.size()), result.data(), size, nullptr, nullptr)) return {};
    return result;
}

DWORD ValidateTarget(const std::wstring& path) {
    if (!ValidPathShape(path) || Utf8(path).empty()) return ERROR_BAD_PATHNAME;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error ? error : ERROR_FILE_NOT_FOUND;
    }
    return ERROR_SUCCESS;
}

DWORD RuntimePath(std::wstring& directory, std::wstring& executable) {
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return ERROR_BAD_PATHNAME;
    module.resize(length);
    const auto separator = module.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return ERROR_BAD_PATHNAME;
    directory = module.substr(0, separator) + L"\\quicklook";
    const DWORD attrs = GetFileAttributesW(directory.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return ERROR_FILE_NOT_FOUND;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
    executable = directory + L"\\Pulse.QuickLook.Host.exe";
    for (const auto* name : {L"Pulse.QuickLook.Host.exe", L"QuickLook.Common.dll"}) {
        const DWORD file_attrs = GetFileAttributesW((directory + L"\\" + name).c_str());
        if (file_attrs == INVALID_FILE_ATTRIBUTES || (file_attrs & FILE_ATTRIBUTE_DIRECTORY))
            return ERROR_FILE_NOT_FOUND;
        if (file_attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
    }
    const DWORD plugins = GetFileAttributesW((directory + L"\\QuickLook.Plugin").c_str());
    if (plugins == INVALID_FILE_ATTRIBUTES || !(plugins & FILE_ATTRIBUTE_DIRECTORY))
        return ERROR_FILE_NOT_FOUND;
    return plugins & FILE_ATTRIBUTE_REPARSE_POINT ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}

bool MakePipe(bool parent_writes, Handle& parent, Handle& child) {
    const std::wstring name = L"\\\\.\\pipe\\Pulse.QuickLook." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(next_pipe.fetch_add(1));
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return false;
    Handle token(raw_token);
    DWORD token_bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &token_bytes);
    if (!token_bytes || token_bytes > 65536) return false;
    std::vector<unsigned char> token_user(token_bytes);
    if (!GetTokenInformation(token.value, TokenUser, token_user.data(), token_bytes, &token_bytes)) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(token_user.data());
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sid_text)) return false;
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid_text) + L")";
    LocalFree(sid_text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return false;
    SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), descriptor, FALSE};
    parent.Reset(CreateNamedPipeW(name.c_str(), FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED |
        (parent_writes ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND),
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &pipe_security));
    const DWORD create_error = GetLastError();
    LocalFree(descriptor);
    if (!parent.valid()) { SetLastError(create_error); return false; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    child.Reset(CreateFileW(name.c_str(), parent_writes ? GENERIC_READ : GENERIC_WRITE, 0,
        &security, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!child.valid()) return false;
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(parent.value, &client_pid) || client_pid != GetCurrentProcessId()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    // Opening our inherited client endpoint has already connected the instance.
    OVERLAPPED connection{};
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;
    connection.hEvent = event.value;
    if (ConnectNamedPipe(parent.value, &connection)) return true;
    return GetLastError() == ERROR_PIPE_CONNECTED;
}

struct HostProcess {
    Handle process;
    Handle job;
    Handle input;
    Handle output;
    Handle read_event;
    OVERLAPPED read{};
    std::array<char, 4096> read_buffer{};
    bool read_pending = false;
    std::string lines;

    ~HostProcess() {
        input.Reset();
        if (read_pending) {
            CancelIoEx(output.value, &read);
            DWORD ignored = 0;
            GetOverlappedResult(output.value, &read, &ignored, TRUE);
        }
        // Only the preview host belongs to this job; user-opened applications
        // may outlive their preview through SILENT_BREAKAWAY_OK.
        job.Reset();
    }

    DWORD Start(HWND owner, uint64_t generation, const std::wstring& path, HANDLE cancelled) {
        std::wstring directory, executable;
        DWORD error = RuntimePath(directory, executable);
        if (error) return error;
        if (!OwnerValid(owner)) return ERROR_INVALID_WINDOW_HANDLE;
        Handle child_input, child_output;
        if (!MakePipe(true, input, child_input) || !MakePipe(false, output, child_output))
            return GetLastError();
        read_event.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        job.Reset(CreateJobObjectW(nullptr, nullptr));
        if (!read_event.valid() || !job.valid()) return GetLastError();
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return GetLastError();

        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return GetLastError();
        const std::array<HANDLE, 2> inherited{child_input.value, child_output.value};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr)) {
            error = GetLastError();
            DeleteProcThreadAttributeList(attributes);
            return error;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_input.value;
        startup.StartupInfo.hStdOutput = child_output.value;
        startup.StartupInfo.hStdError = child_output.value;
        startup.lpAttributeList = attributes;
        std::wstring command = L"\"" + executable + L"\" " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(owner)) + L" " +
            std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(generation) + L" --popup";
        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            directory.c_str(), &startup.StartupInfo, &info);
        error = created ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(attributes);
        if (!created) return error;
        process.Reset(info.hProcess);
        Handle primary_thread(info.hThread);
        if (!AssignProcessToJobObject(job.value, process.value)) {
            error = GetLastError();
            TerminateProcess(process.value, error);
            return error;
        }
        if (ResumeThread(primary_thread.value) == static_cast<DWORD>(-1)) return GetLastError();
        child_input.Reset();
        child_output.Reset();
        return Write(Utf8(path) + "\n", cancelled);
    }

    DWORD Write(const std::string& data, HANDLE cancelled) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return GetLastError();
        const ULONGLONG deadline = GetTickCount64() + kCommandTimeout;
        size_t offset = 0;
        while (offset < data.size()) {
            OVERLAPPED operation{};
            operation.hEvent = event.value;
            ResetEvent(event.value);
            DWORD written = 0;
            const DWORD size = static_cast<DWORD>(std::min<size_t>(16384, data.size() - offset));
            if (!WriteFile(input.value, data.data() + offset, size, &written, &operation)) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) return error;
                const std::array<HANDLE, 3> waits{cancelled, process.value, event.value};
                const ULONGLONG now = GetTickCount64();
                const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(),
                    FALSE, now >= deadline ? 0 : static_cast<DWORD>(deadline - now));
                if (wait != WAIT_OBJECT_0 + 2) {
                    CancelIoEx(input.value, &operation);
                    DWORD ignored = 0;
                    GetOverlappedResult(input.value, &operation, &ignored, TRUE);
                    return wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
                }
                if (!GetOverlappedResult(input.value, &operation, &written, FALSE)) return GetLastError();
            }
            if (!written) return ERROR_BROKEN_PIPE;
            offset += written;
        }
        return ERROR_SUCCESS;
    }

    DWORD BeginRead(DWORD& immediate) {
        immediate = 0;
        read = {};
        read.hEvent = read_event.value;
        ResetEvent(read_event.value);
        if (ReadFile(output.value, read_buffer.data(), static_cast<DWORD>(read_buffer.size()), &immediate, &read))
            return ERROR_SUCCESS;
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) { read_pending = true; return ERROR_SUCCESS; }
        return error;
    }
};

template<class T> bool ParseNumber(std::string_view field, T& number) {
    const auto result = std::from_chars(field.data(), field.data() + field.size(), number);
    return result.ec == std::errc{} && result.ptr == field.data() + field.size();
}

std::vector<std::string_view> Fields(std::string_view line) {
    std::vector<std::string_view> fields;
    while (true) {
        const auto tab = line.find('\t');
        fields.push_back(line.substr(0, tab));
        if (tab == std::string_view::npos) break;
        line.remove_prefix(tab + 1);
    }
    return fields;
}

} // namespace

struct QuickLookIntegration::State : std::enable_shared_from_this<State> {
    enum class Action { Toggle, Switch, Close };
    struct Request { Action action; uint64_t generation; std::wstring path; };
    std::mutex mutex;
    std::atomic<HWND> owner{nullptr};
    UINT message = 0;
    Handle cancelled{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::optional<Request> request;
    std::wstring selected;
    std::unordered_map<UINT_PTR, QuickLookEvent> events;
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> active{false};
    std::atomic<bool> pending{false};
    std::atomic<bool> running{false};
    uint64_t failed_generation = 0;

    bool Cancelled() const { return WaitForSingleObject(cancelled.value, 0) == WAIT_OBJECT_0; }

    std::optional<Request> TakeRequest() {
        std::lock_guard lock(mutex);
        auto next = std::move(request);
        request.reset();
        return next;
    }

    void Emit(QuickLookEvent event) {
        std::lock_guard lock(mutex);
        if (!owner || event.generation != generation.load() || Cancelled()) return;
        event.path = selected;
        if (event.kind == QuickLookEventKind::Failed) {
            if (failed_generation == event.generation) return;
            failed_generation = event.generation;
            active = false;
            pending = false;
        }
        if (event.kind == QuickLookEventKind::Stopped) { active = false; pending = false; }
        // Coalesce state notifications without dropping navigation or failures.
        if (event.kind == QuickLookEventKind::Visibility || event.kind == QuickLookEventKind::Ready) {
            for (auto i = events.begin(); i != events.end();) {
                if (i->second.kind == QuickLookEventKind::Visibility || i->second.kind == QuickLookEventKind::Ready)
                    i = events.erase(i);
                else ++i;
            }
        }
        if (events.size() >= 64) events.clear();
        UINT_PTR token = next_token.fetch_add(1);
        if (!token) token = next_token.fetch_add(1);
        events.emplace(token, std::move(event));
        if (!PostMessageW(owner, message, 0, static_cast<LPARAM>(token))) events.erase(token);
    }

    void Fail(uint64_t id, DWORD error) {
        QuickLookEvent event;
        event.kind = QuickLookEventKind::Failed;
        event.generation = id;
        event.error = error ? error : ERROR_GEN_FAILURE;
        Emit(std::move(event));
    }

    void Stopped(uint64_t id) {
        QuickLookEvent event;
        event.kind = QuickLookEventKind::Stopped;
        event.generation = id;
        Emit(std::move(event));
    }

    void Run() {
        std::unique_ptr<HostProcess> host;
        bool ready = false;
        bool had_ready_state = false;
        ULONGLONG deadline = 0;
        uint64_t issued = 0;
        std::optional<Request> deferred;

        const auto consume = [&](std::string_view line) {
            if (line == "READY") { ready = true; return; }
            const auto fields = Fields(line);
            uint64_t id = 0;
            if (fields.size() < 2 || !ParseNumber(fields[1], id) || id != generation.load()) return;
            if (fields[0] == "STATE" && fields.size() == 5) {
                int visible = 0, count = 0, busy = 0;
                if (!ParseNumber(fields[2], visible) || !ParseNumber(fields[3], count) ||
                    !ParseNumber(fields[4], busy) || (visible != 0 && visible != 1) ||
                    (busy != 0 && busy != 1) || count < 0 || count > 256) return;
                {
                    std::lock_guard lock(mutex);
                    if (id != generation.load() || failed_generation == id) return;
                    active = visible != 0;
                    pending = busy != 0;
                }
                if (!busy) deadline = 0;
                QuickLookEvent event;
                event.kind = visible && !busy && !had_ready_state ? QuickLookEventKind::Ready : QuickLookEventKind::Visibility;
                if (visible && !busy) had_ready_state = true;
                event.generation = id;
                event.preview_visible = visible != 0;
                event.window_count = count;
                Emit(std::move(event));
            } else if (fields[0] == "ERROR" && fields.size() == 3) {
                const DWORD error = fields[2] == "TARGET_UNAVAILABLE" ? ERROR_FILE_NOT_FOUND :
                    fields[2] == "UNSUPPORTED" ? ERROR_NOT_SUPPORTED : ERROR_INVALID_DATA;
                Fail(id, error);
                deadline = 0;
            } else if (fields[0] == "NAV" && fields.size() == 3) {
                int direction = 0;
                if (!ParseNumber(fields[2], direction) || (direction != -1 && direction != 1)) return;
                QuickLookEvent event;
                event.kind = QuickLookEventKind::Navigate;
                event.generation = id;
                event.direction = direction;
                Emit(std::move(event));
            }
        };
        const auto append = [&](DWORD bytes) -> bool {
            if (!bytes) return false;
            host->lines.append(host->read_buffer.data(), bytes);
            size_t newline = 0;
            while ((newline = host->lines.find('\n')) != std::string::npos) {
                if (newline > kMaxProtocolLine) return false;
                std::string line = host->lines.substr(0, newline);
                host->lines.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                consume(line);
            }
            return host->lines.size() <= kMaxProtocolLine;
        };

        while (!Cancelled()) {
            if (!OwnerValid(owner)) break;
            if (auto latest = TakeRequest()) deferred = std::move(latest);
            if (!host && deferred) {
                Request next = std::move(*deferred);
                deferred.reset();
                if (next.action == Action::Close) { Stopped(next.generation); continue; }
                const DWORD target_error = ValidateTarget(next.path);
                if (target_error) { Fail(next.generation, target_error); continue; }
                if (Cancelled() || next.generation != generation.load()) continue;
                host = std::make_unique<HostProcess>();
                const DWORD start_error = host->Start(owner, next.generation, next.path, cancelled.value);
                if (start_error) { host.reset(); Fail(next.generation, start_error); continue; }
                running = true;
                ready = false;
                had_ready_state = false;
                issued = next.generation;
                deadline = GetTickCount64() + kStartupTimeout;
            }
            if (host && deferred && (!ready && deferred->action == Action::Close)) {
                issued = deferred->generation;
                deferred.reset();
                host.reset();
                running = false;
                Stopped(issued);
                continue;
            }
            if (host && ready && deferred) {
                Request next = std::move(*deferred);
                deferred.reset();
                if (next.action != Action::Close) {
                    const DWORD target_error = ValidateTarget(next.path);
                    if (target_error) { Fail(next.generation, target_error); continue; }
                    if (Cancelled() || next.generation != generation.load()) continue;
                }
                const std::string action = next.action == Action::Toggle ? "TOGGLE" : next.action == Action::Switch ? "SWITCH" : "CLOSE";
                std::string command = action + "\t" + std::to_string(next.generation);
                if (next.action != Action::Close) command += "\t" + Utf8(next.path);
                command += '\n';
                const DWORD write_error = host->Write(command, cancelled.value);
                issued = next.generation;
                if (write_error) {
                    host.reset();
                    running = false;
                    if (next.action == Action::Close) Stopped(issued); else Fail(issued, write_error);
                    continue;
                }
                deadline = GetTickCount64() + kCommandTimeout;
            }
            if (!host) {
                const std::array<HANDLE, 2> waits{cancelled.value, wake.value};
                WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 1000);
                continue;
            }
            if (!host->read_pending) {
                DWORD immediate = 0;
                const DWORD error = host->BeginRead(immediate);
                if (error || (immediate && !append(immediate))) {
                    DWORD exit_code = 1;
                    const bool exited = WaitForSingleObject(host->process.value, 0) == WAIT_OBJECT_0;
                    if (exited) GetExitCodeProcess(host->process.value, &exit_code);
                    host.reset();
                    running = false;
                    if (exited && !exit_code && ready && !pending) Stopped(issued);
                    else if (active || pending) Fail(issued, error ? error : ERROR_INVALID_DATA);
                    else Stopped(issued);
                    continue;
                }
                if (immediate) continue;
            }
            const std::array<HANDLE, 4> waits{cancelled.value, wake.value, host->read_event.value, host->process.value};
            const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 100);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_OBJECT_0 + 2) {
                DWORD bytes = 0;
                const BOOL result = GetOverlappedResult(host->output.value, &host->read, &bytes, FALSE);
                host->read_pending = false;
                if (!result || !append(bytes)) {
                    const DWORD error = result ? ERROR_INVALID_DATA : GetLastError();
                    host.reset();
                    running = false;
                    if (active || pending) Fail(issued, error); else Stopped(issued);
                }
            } else if (wait == WAIT_OBJECT_0 + 3) {
                DWORD exit_code = 1;
                GetExitCodeProcess(host->process.value, &exit_code);
                host.reset();
                running = false;
                if ((!ready || pending || exit_code) && (active || pending)) Fail(issued, ERROR_PROCESS_ABORTED);
                else Stopped(issued);
            } else if (wait == WAIT_FAILED) {
                const DWORD error = GetLastError();
                host.reset();
                running = false;
                Fail(issued, error);
            }
            if (host && deadline && GetTickCount64() >= deadline) {
                host.reset();
                running = false;
                if (active || pending) Fail(generation.load(), ERROR_TIMEOUT); else Stopped(generation.load());
            }
        }
        host.reset();
        running = false;
    }
};

QuickLookIntegration::~QuickLookIntegration() { Shutdown(); }

bool QuickLookIntegration::Initialize(HWND owner, UINT notify_message) {
    if (state_) return state_->owner == owner && state_->message == notify_message;
    if (!OwnerValid(owner) || notify_message < WM_APP || notify_message > 0xBFFF) return false;
    auto state = std::make_shared<State>();
    if (!state->cancelled.valid() || !state->wake.valid()) return false;
    state->owner = owner;
    state->message = notify_message;
    try {
        std::thread([state] {
            try { state->Run(); }
            catch (...) { state->running = false; state->Fail(state->generation.load(), ERROR_GEN_FAILURE); }
        }).detach();
    } catch (...) { return false; }
    state_ = std::move(state);
    return true;
}

bool QuickLookIntegration::Submit(const std::wstring& path, bool toggle) {
    auto state = state_;
    if (!state || !ValidPathShape(path) || (!toggle && !state->active)) return false;
    {
        std::lock_guard lock(state->mutex);
        if (!state->owner || state->Cancelled()) return false;
        const uint64_t id = state->generation.fetch_add(1) + 1;
        state->selected = path;
        state->active = true;
        state->pending = true;
        state->events.clear();
        state->request = State::Request{toggle ? State::Action::Toggle : State::Action::Switch, id, path};
    }
    SetEvent(state->wake.value);
    return true;
}

bool QuickLookIntegration::Toggle(const std::wstring& path) { return Submit(path, true); }
bool QuickLookIntegration::Switch(const std::wstring& path) { return Submit(path, false); }

void QuickLookIntegration::Close() {
    auto state = state_;
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        const uint64_t id = state->generation.fetch_add(1) + 1;
        state->active = false;
        state->pending = false;
        state->events.clear();
        state->request = State::Request{State::Action::Close, id, {}};
    }
    SetEvent(state->wake.value);
}

void QuickLookIntegration::Shutdown() {
    auto state = std::move(state_);
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        state->owner = nullptr;
        state->events.clear();
        state->request.reset();
        state->active = false;
        state->pending = false;
    }
    SetEvent(state->cancelled.value);
}

bool QuickLookIntegration::active() const noexcept { return state_ && state_->active.load(); }
bool QuickLookIntegration::pending() const noexcept { return state_ && state_->pending.load(); }
bool QuickLookIntegration::running() const noexcept { return state_ && state_->running.load(); }
uint64_t QuickLookIntegration::generation() const noexcept { return state_ ? state_->generation.load() : 0; }
std::wstring QuickLookIntegration::path() const {
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    return state_->selected;
}

bool QuickLookIntegration::TakeEvent(UINT_PTR token, QuickLookEvent& event) {
    if (!state_) return false;
    std::lock_guard lock(state_->mutex);
    const auto found = state_->events.find(token);
    if (found == state_->events.end()) return false;
    event = std::move(found->second);
    state_->events.erase(found);
    return event.generation == state_->generation.load() && state_->owner && !state_->Cancelled();
}

} // namespace pulse::app
