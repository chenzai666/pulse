#include "file_conversion_integration.h"
#include "file_conversion_protocol.h"
#include "../common/current_user_security.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pulse::app {
namespace {

using file_conversion::Action;
using file_conversion::Request;
constexpr wchar_t kOwnerProperty[] = L"Pulse.FileConversion.Session";
constexpr DWORD kCommandTimeout = 15000;
constexpr ULONGLONG kResponseTimeout = 30000;
constexpr ULONGLONG kConversionSilenceTimeout = 10 * 60 * 1000;
std::atomic<UINT_PTR> next_token{1};
std::atomic<uint64_t> next_generation{1};
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

UINT_PTR NewToken() {
    UINT_PTR token = next_token.fetch_add(1);
    return token ? token : next_token.fetch_add(1);
}

uint64_t NewGeneration() {
    uint64_t generation = next_generation.fetch_add(1);
    return generation ? generation : next_generation.fetch_add(1);
}

bool OwnerValid(HWND owner, UINT_PTR cookie = 0) {
    DWORD pid = 0;
    return owner && IsWindow(owner) && GetWindowThreadProcessId(owner, &pid) && pid == GetCurrentProcessId() &&
        (!cookie || GetPropW(owner, kOwnerProperty) == reinterpret_cast<HANDLE>(cookie));
}

DWORD RuntimePath(std::wstring& directory, std::wstring& executable) {
    std::wstring module(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return ERROR_BAD_PATHNAME;
    module.resize(length);
    const auto slash = module.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return ERROR_BAD_PATHNAME;
    directory = module.substr(0, slash) + L"\\conversion";
    const DWORD attrs = GetFileAttributesW(directory.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_FILE_NOT_FOUND;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
    executable = directory + L"\\Pulse.Filee.Host.exe";
    const DWORD file_attrs = GetFileAttributesW(executable.c_str());
    if (file_attrs == INVALID_FILE_ATTRIBUTES || (file_attrs & FILE_ATTRIBUTE_DIRECTORY)) return ERROR_FILE_NOT_FOUND;
    if (file_attrs & FILE_ATTRIBUTE_REPARSE_POINT) return ERROR_ACCESS_DENIED;
    return file_conversion::AbsolutePathShape(executable) ? ERROR_SUCCESS : ERROR_BAD_PATHNAME;
}

bool MakePipe(bool parent_writes, Handle& parent, Handle& child) {
    const std::wstring name = L"\\\\.\\pipe\\Pulse.Filee." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(next_pipe.fetch_add(1));
    CurrentUserSecurityAttributes security;
    if (!security) { SetLastError(ERROR_ACCESS_DENIED); return false; }
    parent.Reset(CreateNamedPipeW(name.c_str(), FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED |
        (parent_writes ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND),
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, security.get()));
    if (!parent.valid()) return false;
    SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
    child.Reset(CreateFileW(name.c_str(), parent_writes ? GENERIC_READ : GENERIC_WRITE, 0, &inherited,
        OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!child.valid()) return false;
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(parent.value, &client_pid) || client_pid != GetCurrentProcessId()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    OVERLAPPED connection{};
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) return false;
    connection.hEvent = event.value;
    if (ConnectNamedPipe(parent.value, &connection)) return true;
    return GetLastError() == ERROR_PIPE_CONNECTED;
}

struct HostProcess {
    Handle process, job, input, output, read_event;
    OVERLAPPED read{};
    std::array<char, 8192> read_buffer{};
    bool read_pending = false;
    bool command_broken = false;
    std::string lines;

    ~HostProcess() {
        if (job.valid()) TerminateJobObject(job.value, ERROR_OPERATION_ABORTED);
        input.Reset();
        if (read_pending) {
            CancelIoEx(output.value, &read);
            DWORD ignored = 0;
            GetOverlappedResult(output.value, &read, &ignored, TRUE);
        }
        job.Reset();
        if (process.valid()) WaitForSingleObject(process.value, 1000);
    }

    DWORD Start(HWND owner, UINT_PTR cookie, HANDLE stopped) {
        std::wstring directory, executable;
        DWORD error = RuntimePath(directory, executable);
        if (error) return error;
        if (!OwnerValid(owner, cookie)) return ERROR_INVALID_WINDOW_HANDLE;
        Handle child_input, child_output;
        if (!MakePipe(true, input, child_input) || !MakePipe(false, output, child_output)) return GetLastError();
        SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
        Handle error_output(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherited, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        read_event.Reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        job.Reset(CreateJobObjectW(nullptr, nullptr));
        if (!error_output.valid() || !read_event.valid() || !job.valid()) return GetLastError();
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        // Every Filee engine stays in this job, including grandchildren.
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return GetLastError();
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return GetLastError();
        const std::array<HANDLE, 3> whitelist{child_input.value, child_output.value, error_output.value};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(whitelist.data()), sizeof(whitelist), nullptr, nullptr)) {
            error = GetLastError();
            DeleteProcThreadAttributeList(attributes);
            return error;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_input.value;
        startup.StartupInfo.hStdOutput = child_output.value;
        startup.StartupInfo.hStdError = error_output.value;
        startup.lpAttributeList = attributes;
        std::wstring command = L"\"" + executable + L"\" --owner-pid " + std::to_wstring(GetCurrentProcessId());
        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, directory.c_str(), &startup.StartupInfo, &info);
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
        if (!OwnerValid(owner, cookie) || WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0)
            return ERROR_OPERATION_ABORTED;
        if (ResumeThread(primary_thread.value) == static_cast<DWORD>(-1)) return GetLastError();
        return ERROR_SUCCESS;
    }

    DWORD Write(const std::string& data, HANDLE stopped, HANDLE interrupted, DWORD timeout = kCommandTimeout) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) return GetLastError();
        const ULONGLONG deadline = GetTickCount64() + timeout;
        size_t offset = 0;
        while (offset < data.size()) {
            if (WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0 ||
                WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0) return ERROR_OPERATION_ABORTED;
            OVERLAPPED operation{};
            operation.hEvent = event.value;
            ResetEvent(event.value);
            DWORD written = 0;
            const DWORD size = static_cast<DWORD>((std::min)(size_t{16384}, data.size() - offset));
            if (!WriteFile(input.value, data.data() + offset, size, &written, &operation)) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING) return error;
                const std::array<HANDLE, 4> waits{stopped, interrupted, process.value, event.value};
                const ULONGLONG now = GetTickCount64();
                const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE,
                    now >= deadline ? 0 : static_cast<DWORD>(deadline - now));
                if (result != WAIT_OBJECT_0 + 3) {
                    CancelIoEx(input.value, &operation);
                    DWORD ignored = 0;
                    GetOverlappedResult(input.value, &operation, &ignored, TRUE);
                    return result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
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
            return immediate ? ERROR_SUCCESS : ERROR_BROKEN_PIPE;
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) { read_pending = true; return ERROR_SUCCESS; }
        return error;
    }

    void DrainForCleanup(HANDLE stopped, HANDLE interrupted) {
        // Let cooperative cancellation flush terminal JSON and clean its
        // staging files; none of these retired-host events reach the UI.
        const ULONGLONG deadline = GetTickCount64() + 500;
        while (GetTickCount64() < deadline) {
            if (WaitForSingleObject(stopped, 0) == WAIT_OBJECT_0 ||
                WaitForSingleObject(interrupted, 0) == WAIT_OBJECT_0) return;
            if (!read_pending) {
                DWORD immediate = 0;
                if (BeginRead(immediate)) return;
                if (immediate) continue;
            }
            const std::array<HANDLE, 4> waits{stopped, interrupted, process.value, read_event.value};
            const ULONGLONG now = GetTickCount64();
            const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE,
                now >= deadline ? 0 : static_cast<DWORD>(deadline - now));
            if (result != WAIT_OBJECT_0 + 3) return;
            DWORD ignored = 0;
            const BOOL ok = GetOverlappedResult(output.value, &read, &ignored, FALSE);
            read_pending = false;
            if (!ok || !ignored) return;
        }
    }
};

DWORD ValidateRequest(const Request& request) {
    if (request.inputs.empty() || request.inputs.size() > file_conversion::kMaxInputs) return ERROR_INVALID_PARAMETER;
    for (const auto& path : request.inputs)
        if (!file_conversion::AbsolutePathShape(path)) return ERROR_BAD_PATHNAME;
    if (request.action == Action::Convert && !file_conversion::AbsolutePathShape(request.output_directory))
        return ERROR_BAD_PATHNAME;
    return ERROR_SUCCESS;
}

} // namespace

struct FileConversionIntegration::State {
    struct Commands {
        std::optional<Request> caps, start;
        uint64_t cancel_job = 0;
        bool close = false;
    };
    std::mutex mutex;
    std::atomic<HWND> owner{nullptr};
    UINT_PTR cookie = 0;
    UINT message = 0;
    Handle stopped{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle interrupted{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Commands queued;
    std::unordered_map<UINT_PTR, FileConversionEvent> events;
    std::atomic<uint64_t> caps_generation{0}, job_generation{0};
    std::atomic<bool> busy{false}, running{false};
    std::atomic<bool> accepting{true};

    bool Retired() const {
        return WaitForSingleObject(stopped.value, 0) == WAIT_OBJECT_0 || !OwnerValid(owner.load(), cookie);
    }

    Commands Pull() {
        std::lock_guard lock(mutex);
        Commands commands = std::move(queued);
        queued = Commands{};
        ResetEvent(interrupted.value);
        return commands;
    }

    bool ControlPending() {
        std::lock_guard lock(mutex);
        return queued.close || queued.cancel_job != 0;
    }

    bool Relevant(const FileConversionEvent& event) const {
        if (event.kind == FileConversionEventKind::Capabilities) return event.generation == caps_generation.load();
        if (event.kind == FileConversionEventKind::Failed)
            return event.generation == caps_generation.load() || event.generation == job_generation.load();
        return event.generation == job_generation.load();
    }

    void Emit(FileConversionEvent event, const Request* request = nullptr) {
        if (request) { event.inputs = request->inputs; event.target = request->target; }
        std::lock_guard lock(mutex);
        if (Retired() || !Relevant(event)) return;
        if (event.generation == job_generation.load() && (event.kind == FileConversionEventKind::Completed ||
            event.kind == FileConversionEventKind::Failed || event.kind == FileConversionEventKind::Stopped)) busy = false;
        for (auto it = events.begin(); it != events.end();) {
            if (!Relevant(it->second) || (event.kind == FileConversionEventKind::Progress &&
                it->second.kind == FileConversionEventKind::Progress && it->second.generation == event.generation))
                it = events.erase(it);
            else ++it;
        }
        if (events.size() >= 32) events.clear();
        const UINT_PTR token = NewToken();
        events.emplace(token, std::move(event));
        if (!PostMessageW(owner.load(), message, 0, static_cast<LPARAM>(token))) events.erase(token);
    }

    void Fail(const Request& request, DWORD code) {
        FileConversionEvent event;
        event.kind = FileConversionEventKind::Failed;
        event.generation = request.generation;
        event.error = code ? code : ERROR_GEN_FAILURE;
        event.error_key = code == ERROR_FILE_NOT_FOUND ? L"host_unavailable" : code == ERROR_TIMEOUT ? L"host_timeout" :
            code == ERROR_BAD_PATHNAME ? L"invalid_path" : L"host_failed";
        Emit(std::move(event), &request);
    }

    void Run() {
        std::unique_ptr<HostProcess> host;
        std::optional<Request> deferred_caps, deferred_job, issued_caps, issued_job;
        ULONGLONG caps_deadline = 0, job_deadline = 0;
        bool job_started = false;
        const auto reset_host = [&] {
            host.reset();
            running = false;
            caps_deadline = job_deadline = 0;
            job_started = false;
        };
        const auto fail_host = [&](DWORD error) {
            reset_host();
            if (issued_caps) { Fail(*issued_caps, error); issued_caps.reset(); }
            if (issued_job) { Fail(*issued_job, error); issued_job.reset(); }
        };
        const auto consume = [&](std::string_view line) -> bool {
            FileConversionEvent event;
            if (!file_conversion::DecodeEvent(line, event)) return false;
            if (issued_caps && event.generation == issued_caps->generation &&
                (event.kind == FileConversionEventKind::Capabilities || event.kind == FileConversionEventKind::Failed)) {
                Emit(std::move(event), &*issued_caps);
                issued_caps.reset();
                caps_deadline = 0;
            } else if (issued_job && event.generation == issued_job->generation &&
                       event.kind != FileConversionEventKind::Capabilities) {
                const auto kind = event.kind;
                if (kind == FileConversionEventKind::Started && job_started) return true;
                job_started = true;
                job_deadline = GetTickCount64() + kConversionSilenceTimeout;
                Emit(std::move(event), &*issued_job);
                if (kind == FileConversionEventKind::Completed || kind == FileConversionEventKind::Failed) {
                    issued_job.reset();
                    job_deadline = 0;
                }
            }
            return true; // obsolete capabilities and control replies never affect a job
        };
        const auto append = [&](DWORD bytes) -> bool {
            if (!bytes) return false;
            host->lines.append(host->read_buffer.data(), bytes);
            size_t newline = 0;
            while ((newline = host->lines.find('\n')) != std::string::npos) {
                if (newline > file_conversion::kMaxProtocolLine) return false;
                std::string line = host->lines.substr(0, newline);
                host->lines.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!consume(line)) return false;
            }
            return host->lines.size() <= file_conversion::kMaxProtocolLine;
        };
        while (!Retired()) {
            Commands commands = Pull();
            if (commands.cancel_job != job_generation.load()) commands.cancel_job = 0;
            if (commands.close || commands.cancel_job) {
                std::optional<Request> cancelled_job = issued_job ? issued_job : deferred_job ? deferred_job : commands.start;
                if (host && !host->command_broken) {
                    Request control;
                    control.action = commands.close ? Action::Shutdown : Action::Cancel;
                    control.generation = NewGeneration();
                    control.job_id = commands.cancel_job;
                    std::string line;
                    if (file_conversion::EncodeRequest(control, line) &&
                        host->Write(line, stopped.value, interrupted.value, 250) == ERROR_SUCCESS)
                        host->DrainForCleanup(stopped.value, interrupted.value);
                }
                reset_host(); // kill the complete job tree even if the host ignores cancellation
                issued_job.reset();
                deferred_job.reset();
                if (!commands.close && issued_caps && issued_caps->generation == caps_generation.load())
                    deferred_caps = std::move(issued_caps);
                issued_caps.reset();
                if (commands.close) { deferred_caps.reset(); commands.start.reset(); }
                if (cancelled_job) {
                    FileConversionEvent event;
                    event.kind = FileConversionEventKind::Stopped;
                    event.generation = cancelled_job->generation;
                    event.error = ERROR_OPERATION_ABORTED;
                    event.error_key = L"cancelled";
                    Emit(std::move(event), &*cancelled_job);
                }
            }
            if (commands.caps && commands.caps->generation == caps_generation.load()) deferred_caps = std::move(commands.caps);
            if (commands.start && commands.start->generation == job_generation.load() && !commands.cancel_job)
                deferred_job = std::move(commands.start);
            if (deferred_caps && deferred_caps->generation != caps_generation.load()) deferred_caps.reset();
            for (auto* request : {&deferred_caps, &deferred_job}) {
                if (*request) {
                    const DWORD error = ValidateRequest(**request);
                    if (error) { Fail(**request, error); request->reset(); }
                }
            }
            if (!host && (deferred_caps || deferred_job)) {
                host = std::make_unique<HostProcess>();
                const DWORD error = host->Start(owner.load(), cookie, stopped.value);
                if (error) {
                    reset_host();
                    if (deferred_caps) { Fail(*deferred_caps, error); deferred_caps.reset(); }
                    if (deferred_job) { Fail(*deferred_job, error); deferred_job.reset(); }
                    continue;
                }
                running = true;
            }
            const auto issue = [&](std::optional<Request>& deferred, std::optional<Request>& issued,
                                   ULONGLONG& deadline) -> DWORD {
                if (!deferred) return ERROR_SUCCESS;
                Request request = std::move(*deferred);
                deferred.reset();
                DWORD error = ValidateRequest(request);
                std::string line;
                if (!error && !file_conversion::EncodeRequest(request, line)) error = ERROR_INVALID_PARAMETER;
                if (error) { Fail(request, error); return ERROR_SUCCESS; }
                issued = std::move(request);
                if (issued->action == Action::Convert) job_started = false;
                deadline = GetTickCount64() + kResponseTimeout;
                error = host->Write(line, stopped.value, interrupted.value);
                if (error) host->command_broken = true;
                return error;
            };
            if (host) {
                // New capabilities supersede only capabilities, never issued_job.
                DWORD error = issue(deferred_caps, issued_caps, caps_deadline);
                if (!error && !issued_job) error = issue(deferred_job, issued_job, job_deadline);
                if (error) {
                    if (error == ERROR_OPERATION_ABORTED && ControlPending()) continue;
                    fail_host(error);
                    continue;
                }
                if (!host->read_pending) {
                    DWORD immediate = 0;
                    error = host->BeginRead(immediate);
                    if (error || (immediate && !append(immediate))) {
                        fail_host(error ? error : ERROR_INVALID_DATA);
                        continue;
                    }
                    if (immediate) continue;
                }
                const std::array<HANDLE, 5> waits{stopped.value, wake.value, interrupted.value,
                    host->read_event.value, host->process.value};
                const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 200);
                if (result == WAIT_OBJECT_0) break;
                if (result == WAIT_OBJECT_0 + 3) {
                    DWORD bytes = 0;
                    const BOOL read_ok = GetOverlappedResult(host->output.value, &host->read, &bytes, FALSE);
                    host->read_pending = false;
                    if (!read_ok || !append(bytes)) fail_host(read_ok ? ERROR_INVALID_DATA : GetLastError());
                } else if (result == WAIT_OBJECT_0 + 4) fail_host(ERROR_PROCESS_ABORTED);
                else if (result == WAIT_FAILED) fail_host(GetLastError());
                const ULONGLONG now = GetTickCount64();
                if (host && caps_deadline && now >= caps_deadline) {
                    if (issued_caps) { Fail(*issued_caps, ERROR_TIMEOUT); issued_caps.reset(); }
                    caps_deadline = 0; // a menu timeout must not kill an active conversion
                }
                if (host && job_deadline && now >= job_deadline) fail_host(ERROR_TIMEOUT);
            } else {
                const std::array<HANDLE, 2> waits{stopped.value, wake.value};
                WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 1000);
            }
        }
        reset_host();
        busy = false;
        accepting = false;
    }
};

FileConversionIntegration::~FileConversionIntegration() { Shutdown(); }

bool FileConversionIntegration::Initialize(HWND owner, UINT notify_message) {
    if (state_) {
        if (state_->owner.load() != owner || state_->message != notify_message) return false;
        if (state_->accepting && !state_->Retired()) return true;
        Shutdown(); // a failed worker can be replaced without waiting on the UI thread
    }
    if (!OwnerValid(owner) || notify_message < WM_APP || notify_message > 0xBFFF || GetPropW(owner, kOwnerProperty))
        return false;
    auto state = std::make_shared<State>();
    if (!state->stopped.valid() || !state->interrupted.valid() || !state->wake.valid()) return false;
    state->owner = owner;
    state->message = notify_message;
    state->cookie = NewToken();
    if (!SetPropW(owner, kOwnerProperty, reinterpret_cast<HANDLE>(state->cookie))) return false;
    try {
        std::thread([state] {
            try { state->Run(); }
            catch (...) {
                state->accepting = false;
                state->running = false;
                state->busy = false;
                try {
                    Request request;
                    request.generation = state->job_generation.load();
                    if (request.generation) state->Fail(request, ERROR_GEN_FAILURE);
                    request.generation = state->caps_generation.load();
                    if (request.generation) state->Fail(request, ERROR_GEN_FAILURE);
                } catch (...) {} // an allocation failure must not escape the worker
            }
        }).detach();
    } catch (...) { RemovePropW(owner, kOwnerProperty); return false; }
    state_ = std::move(state);
    return true;
}

uint64_t FileConversionIntegration::Capabilities(std::vector<std::wstring> inputs) {
    const auto state = state_;
    if (!state || inputs.empty() || inputs.size() > file_conversion::kMaxInputs) return 0;
    const uint64_t generation = NewGeneration();
    {
        std::lock_guard lock(state->mutex);
        if (state->Retired() || !state->accepting) return 0;
        state->caps_generation = generation;
        Request request;
        request.action = Action::Capabilities;
        request.generation = generation;
        request.inputs = std::move(inputs);
        state->queued.caps = std::move(request);
    }
    SetEvent(state->wake.value);
    return generation;
}

uint64_t FileConversionIntegration::Start(std::vector<std::wstring> inputs, std::wstring target,
                                          std::wstring output_directory, int quality) {
    const auto state = state_;
    if (!state || inputs.empty() || inputs.size() > file_conversion::kMaxInputs || target.empty() || target.size() > 128 ||
        output_directory.empty() || quality < 1 || quality > 100) return 0;
    const uint64_t generation = NewGeneration();
    {
        std::lock_guard lock(state->mutex);
        if (state->Retired() || !state->accepting || state->busy || state->queued.close) return 0;
        Request request;
        request.action = Action::Convert;
        request.generation = generation;
        request.inputs = std::move(inputs);
        request.target = std::move(target);
        request.output_directory = std::move(output_directory);
        request.quality = quality;
        state->job_generation = generation;
        state->queued.start = std::move(request);
        state->busy = true;
    }
    SetEvent(state->wake.value);
    return generation;
}

void FileConversionIntegration::Cancel() {
    const auto state = state_;
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        if (state->Retired() || !state->busy) return;
        state->queued.cancel_job = state->job_generation.load();
        SetEvent(state->interrupted.value);
    }
    SetEvent(state->wake.value);
}

void FileConversionIntegration::Close() {
    const auto state = state_;
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        if (state->Retired()) return;
        state->caps_generation = NewGeneration();
        state->queued.caps.reset();
        state->queued.close = true;
        state->events.clear();
        SetEvent(state->interrupted.value);
    }
    SetEvent(state->wake.value);
}

void FileConversionIntegration::Shutdown() {
    auto state = std::move(state_);
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        const HWND owner = state->owner.exchange(nullptr);
        if (OwnerValid(owner, state->cookie)) RemovePropW(owner, kOwnerProperty);
        state->events.clear();
        state->queued = State::Commands{};
        state->busy = false;
        SetEvent(state->stopped.value);
        SetEvent(state->interrupted.value);
    }
    SetEvent(state->wake.value);
}

bool FileConversionIntegration::busy() const noexcept { return state_ && state_->busy.load(); }
bool FileConversionIntegration::running() const noexcept { return state_ && state_->running.load(); }

bool FileConversionIntegration::TakeEvent(UINT_PTR token, FileConversionEvent& event) {
    const auto state = state_;
    if (!state) return false;
    std::lock_guard lock(state->mutex);
    const auto found = state->events.find(token);
    if (found == state->events.end()) return false;
    const bool relevant = !state->Retired() && state->Relevant(found->second);
    if (relevant) event = std::move(found->second);
    state->events.erase(found);
    return relevant;
}

} // namespace pulse::app
