#include "native_dialog_takeover.h"
#include "native_file_dialog_adapter.h"
#include "../common/command_line.h"
#include "../common/json_utils.h"
#include "../common/windows_compat.h"

#include <objbase.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using pulse::app::NativeDialogAppearance;
using pulse::app::NativeDialogTakeover;
using pulse::app::NativeFileDialog;

constexpr DWORD kAttachTimeout = 25000;
constexpr DWORD kResultTimeout = 15000;
constexpr size_t kOutputLimit = 128 * 1024;
constexpr wchar_t kDesktopPrefix[] = L"PulseNativeTakeover_";

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    void Reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return "<invalid UTF-16>";
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr) != size) return "<UTF-8 conversion failed>";
    return result;
}

struct TestLog {
    explicit TestLog(const wchar_t* path) : stream(std::filesystem::path(path), std::ios::binary) {}
    bool Check(bool passed, const std::string& label) {
        stream << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
        stream.flush();
        if (!passed) ++failures;
        return passed;
    }
    void Detail(const std::string& value) { stream << "[DETAIL] " << value << '\n'; stream.flush(); }
    std::ofstream stream;
    int failures = 0;
};

bool Until(const std::function<bool()>& condition, DWORD timeout) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        if (condition()) return true;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return condition();
}

std::wstring ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) return {};
    path.resize(size);
    return path;
}

std::wstring ClassName(HWND window) {
    wchar_t name[128]{};
    const int size = GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return size > 0 ? std::wstring(name, static_cast<size_t>(size)) : std::wstring();
}

class ChildProcess {
public:
    ChildProcess(TestLog& log, std::string label) : log_(log), label_(std::move(label)) {}
    ~ChildProcess() {
        if (Running()) {
            log_.Detail(label_ + ": terminate only the fixture/test-host process created by this case during cleanup");
            TerminateProcess(process_.get(), ERROR_PROCESS_ABORTED);
            WaitForSingleObject(process_.get(), 3000);
        }
    }
    bool Start(const std::wstring& executable, const std::vector<std::wstring>& arguments,
        const std::wstring& desktop_name, const std::vector<HANDLE>& extra_handles = {}) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE raw_read = nullptr, raw_write = nullptr;
        if (!CreatePipe(&raw_read, &raw_write, &security, 65536)) return false;
        output_.Reset(raw_read);
        Handle writer(raw_write);
        if (!SetHandleInformation(output_.get(), HANDLE_FLAG_INHERIT, 0)) return false;
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (input.get() == INVALID_HANDLE_VALUE) return false;
        std::vector<HANDLE> inherited{writer.get(), input.get()};
        inherited.insert(inherited.end(), extra_handles.begin(), extra_handles.end());
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size) return false;
        std::vector<unsigned char> storage(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return false;
        struct AttributeScope {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeScope() { DeleteProcThreadAttributeList(value); }
        } scope{attributes};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr)) return false;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.lpDesktop = const_cast<wchar_t*>(desktop_name.c_str());
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.get();
        startup.StartupInfo.hStdOutput = writer.get();
        startup.StartupInfo.hStdError = writer.get();
        startup.lpAttributeList = attributes;
        std::wstring command = pulse::QuoteWindowsArgument(executable);
        for (const auto& argument : arguments) command += L" " + pulse::QuoteWindowsArgument(argument);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
            nullptr, nullptr, &startup.StartupInfo, &info)) return false;
        process_.Reset(info.hProcess);
        Handle primary(info.hThread);
        process_id_ = info.dwProcessId;
        return ResumeThread(primary.get()) != static_cast<DWORD>(-1);
    }
    bool Running() const { return process_.get() && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT; }
    HANDLE process() const { return process_.get(); }
    DWORD id() const { return process_id_; }
    const std::string& bytes() const { return bytes_; }
    bool Drain() {
        if (!output_.get() || eof_) return !io_failed_;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(output_.get(), nullptr, 0, nullptr, &available, nullptr)) {
                eof_ = GetLastError() == ERROR_BROKEN_PIPE;
                io_failed_ = !eof_;
                return !io_failed_;
            }
            if (!available) return true;
            if (bytes_.size() + available > kOutputLimit) { io_failed_ = true; return false; }
            std::array<char, 4096> block{};
            DWORD read = 0;
            if (!ReadFile(output_.get(), block.data(), (std::min)(available, static_cast<DWORD>(block.size())), &read, nullptr)) {
                io_failed_ = true;
                return false;
            }
            bytes_.append(block.data(), read);
        }
    }
    bool Crash() {
        return process_.get() && TerminateProcess(process_.get(), ERROR_PROCESS_ABORTED) &&
            WaitForSingleObject(process_.get(), 5000) == WAIT_OBJECT_0;
    }
private:
    TestLog& log_;
    std::string label_;
    Handle process_, output_;
    DWORD process_id_ = 0;
    std::string bytes_;
    bool eof_ = false, io_failed_ = false;
};

struct SourceWindow {
    HWND window = nullptr;
    DWORD process = 0, thread = 0;
};

bool SourceMatches(const SourceWindow& source) {
    DWORD process = 0;
    return source.window && GetWindowThreadProcessId(source.window, &process) == source.thread &&
        process == source.process && ClassName(source.window) == L"#32770";
}

SourceWindow FindSource(HDESK desktop, DWORD fixture_process) {
    SourceWindow result;
    struct Context { DWORD fixture; SourceWindow* output; } enumeration{fixture_process, &result};
    EnumDesktopWindows(desktop, [](HWND window, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(data);
        DWORD process = 0, owner_process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        const HWND owner = GetWindow(window, GW_OWNER);
        if (process == context.fixture && IsWindowVisible(window) && ClassName(window) == L"#32770" &&
            owner && GetWindowThreadProcessId(owner, &owner_process) && owner_process == context.fixture &&
            ClassName(owner) == L"STATIC") {
            *context.output = {window, process, thread};
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enumeration));
    return result;
}

bool ProcessIdentity(DWORD process_id, const std::wstring& executable, FILETIME& created) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (!process.get()) return false;
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)) return false;
    path.resize(size);
    FILETIME exited{}, kernel{}, user{};
    return pulse::ui::SamePickerPath(path, executable) && GetProcessTimes(process.get(), &created, &exited, &kernel, &user);
}

struct PickerWindow {
    HWND window = nullptr;
    DWORD process = 0, thread = 0;
    FILETIME created{};
    SourceWindow source;
    std::wstring executable;
};

bool PickerMatches(const PickerWindow& picker) {
    DWORD process = 0;
    FILETIME created{};
    return SourceMatches(picker.source) && picker.window &&
        GetWindowThreadProcessId(picker.window, &process) == picker.thread && process == picker.process &&
        ClassName(picker.window) == L"PulseFolderPickerWindow" && GetWindow(picker.window, GW_OWNER) == picker.source.window &&
        ProcessIdentity(process, picker.executable, created) && CompareFileTime(&created, &picker.created) == 0;
}

PickerWindow FindPicker(HDESK desktop, const SourceWindow& source, const std::wstring& executable) {
    PickerWindow result;
    struct Context { const SourceWindow* source; const std::wstring* executable; PickerWindow* output; } enumeration{&source, &executable, &result};
    EnumDesktopWindows(desktop, [](HWND window, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(data);
        if (ClassName(window) != L"PulseFolderPickerWindow" || !IsWindowVisible(window) ||
            GetWindow(window, GW_OWNER) != context.source->window) return TRUE;
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        FILETIME created{};
        if (!thread || !ProcessIdentity(process, *context.executable, created)) return TRUE;
        *context.output = {window, process, thread, created, *context.source, *context.executable};
        return FALSE;
    }, reinterpret_cast<LPARAM>(&enumeration));
    return result;
}

bool SendPicker(const PickerWindow& picker, int control, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    if (!PickerMatches(picker) || (control != 0 && control != 101 && control != 102)) return false;
    const HWND target = control ? GetDlgItem(picker.window, control) : picker.window;
    DWORD process = 0;
    if (!target || GetWindowThreadProcessId(target, &process) != picker.thread || process != picker.process ||
        (control && (!IsChild(picker.window, target) || ClassName(target) != L"Edit"))) return false;
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(target, message, wparam, lparam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 2000, &result) != 0;
}

void Diagnostics(TestLog& log, HDESK desktop, ChildProcess& child, const std::string& branch) {
    log.Detail(branch + ": fixture PID=" + std::to_string(child.id()) + ", running=" + (child.Running() ? "true" : "false"));
    struct Context { TestLog* log; DWORD fixture; } enumeration{&log, child.id()};
    EnumDesktopWindows(desktop, [](HWND window, LPARAM data) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(data);
        DWORD process = 0, owner_process = 0;
        GetWindowThreadProcessId(window, &process);
        const HWND owner = GetWindow(window, GW_OWNER);
        if (owner) GetWindowThreadProcessId(owner, &owner_process);
        if (process != context.fixture && !(ClassName(window) == L"PulseFolderPickerWindow" && owner_process == context.fixture)) return TRUE;
        context.log->Detail("window HWND=" + std::to_string(reinterpret_cast<uintptr_t>(window)) +
            " PID=" + std::to_string(process) + " class=" + Utf8(ClassName(window)) +
            " visible=" + (IsWindowVisible(window) ? "true" : "false") +
            " enabled=" + (IsWindowEnabled(window) ? "true" : "false") +
            " owner=" + std::to_string(reinterpret_cast<uintptr_t>(owner)));
        if (process == context.fixture && ClassName(window) == L"#32770") {
            NativeFileDialog inspected;
            std::wstring reason;
            const bool accepted = pulse::app::InspectNativeFileDialog(window, inspected, reason);
            context.log->Detail("production InspectNativeFileDialog=" + std::string(accepted ? "accepted" : "rejected") + " reason=" + Utf8(reason));
            size_t count = 0;
            struct Controls { TestLog* log; size_t* count; } control_context{context.log, &count};
            EnumChildWindows(window, [](HWND control, LPARAM nested) -> BOOL {
                auto& controls = *reinterpret_cast<Controls*>(nested);
                if (++*controls.count > 80) return FALSE;
                controls.log->Detail("native control class=" + Utf8(ClassName(control)) + " id=" +
                    std::to_string(GetDlgCtrlID(control)) + " visible=" + (IsWindowVisible(control) ? "true" : "false") +
                    " enabled=" + (IsWindowEnabled(control) ? "true" : "false"));
                return TRUE;
            }, reinterpret_cast<LPARAM>(&control_context));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enumeration));
    child.Drain();
    if (!child.bytes().empty()) log.Detail("fixture stdout=" + child.bytes());
}

bool AwaitPicker(TestLog& log, HDESK desktop, ChildProcess& child, const std::wstring& executable,
    const std::string& branch, bool filename, SourceWindow& source, PickerWindow& picker) {
    bool exited_early = false;
    const bool observed = Until([&] {
        child.Drain();
        if (!child.Running()) { exited_early = true; return true; }
        if (!SourceMatches(source)) source = FindSource(desktop, child.id());
        if (!source.window) return false;
        picker = FindPicker(desktop, source, executable);
        if (!picker.window || !GetDlgItem(picker.window, 101)) return false;
        const HWND edit = GetDlgItem(picker.window, 102);
        return filename ? edit && IsWindowEnabled(edit) : !edit;
    }, kAttachTimeout);
    const bool ready = observed && !exited_early;
    log.Check(ready, branch + ": production watcher identifies native dialog and displays owned Pulse picker");
    if (!ready) Diagnostics(log, desktop, child, branch + (exited_early
        ? ": fixture exited before takeover" : ": attach-timeout / native detector compatibility branch"));
    else log.Check(SourceMatches(source) && IsWindowVisible(source.window) && !IsWindowEnabled(source.window),
        branch + ": original native dialog remains visible and is disabled only while picker is open");
    return ready;
}

bool Number(const std::wstring& json, const std::wstring& key, uint32_t& number) {
    size_t position = pulse::json::ValuePosition(json, key);
    if (position == std::wstring::npos || position >= json.size() || json[position] < L'0' || json[position] > L'9') return false;
    uint64_t value = 0;
    while (position < json.size() && json[position] >= L'0' && json[position] <= L'9') {
        value = value * 10 + static_cast<unsigned>(json[position++] - L'0');
        if (value > UINT32_MAX) return false;
    }
    number = static_cast<uint32_t>(value);
    return true;
}

bool ExpectedResult(TestLog& log, HDESK desktop, ChildProcess& child, const std::string& branch,
    const std::wstring& mode, bool cancelled, const std::vector<std::wstring>& expected) {
    const bool finished = Until([&] { child.Drain(); return !child.Running(); }, kResultTimeout);
    if (!log.Check(finished, branch + ": native host returns through its own modal dialog")) {
        Diagnostics(log, desktop, child, branch + ": native-confirmation/result-timeout");
        return false;
    }
    const bool drained = child.Drain();
    DWORD exit_code = 0;
    const bool exited = GetExitCodeProcess(child.process(), &exit_code) && exit_code == 0;
    const auto& bytes = child.bytes();
    const int size = bytes.empty() ? 0 : MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring json(static_cast<size_t>(size), L'\0');
    const bool decoded = size && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), json.data(), size) == size;
    uint32_t hresult = 0, filter = 0;
    const bool valid = drained && exited && decoded && pulse::json::ValidConfigObject(json) &&
        pulse::json::ExtractString(json, L"mode") == mode && Number(json, L"hresult", hresult) &&
        Number(json, L"filter_index", filter) && filter == 0 &&
        hresult == (cancelled ? static_cast<uint32_t>(HRESULT_FROM_WIN32(ERROR_CANCELLED)) : 0);
    const auto paths = valid ? pulse::json::ExtractStringArray(json, L"paths") : std::vector<std::wstring>();
    bool same = paths.size() == expected.size();
    std::vector<bool> used(paths.size(), false);
    for (const auto& wanted : expected) {
        bool found = false;
        for (size_t index = 0; index < paths.size(); ++index) {
            if (!used[index] && pulse::ui::SamePickerPath(paths[index], wanted)) { used[index] = true; found = true; break; }
        }
        same = same && found;
    }
    const bool accepted = valid && same && pulse::json::ExtractString(json, L"status") == (cancelled ? L"cancelled" : L"accepted");
    log.Check(accepted, branch + ": fixture receives the exact expected paths/status from production native adapter");
    log.Detail(branch + ": stdout=" + bytes);
    return accepted;
}

NativeDialogAppearance Appearance() {
    NativeDialogAppearance appearance;
    appearance.language = L"en-US";
    appearance.style.show_hidden = false;
    return appearance;
}

bool SelectThroughPicker(const PickerWindow& picker, const std::wstring& text, bool folder) {
    const int control = folder ? 101 : 102;
    if (!SendPicker(picker, control, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str())) ||
        !SendPicker(picker, control, WM_KEYDOWN, VK_RETURN)) return false;
    if (!folder) return true;
    return Until([&] {
        if (!PickerMatches(picker)) return true;
        return SendPicker(picker, 0, WM_KEYDOWN, VK_RETURN) && !PickerMatches(picker);
    }, kResultTimeout);
}

void RunRound(TestLog& log, HDESK desktop, const std::wstring& desktop_name, const std::wstring& executable,
    const std::wstring& fixture, const std::filesystem::path& root, const std::wstring& mode,
    const std::wstring& text, const std::vector<std::wstring>& expected, bool cancelled = false) {
    const std::string branch = Utf8(mode) + (cancelled ? "-cancel" : "-confirm");
    ChildProcess child(log, branch);
    NativeDialogTakeover watcher(Appearance(), desktop, desktop_name);
    if (!log.Check(child.Start(fixture, {mode, root.wstring()}, desktop_name), branch + ": launch only own fixture on private desktop")) return;
    SourceWindow source;
    PickerWindow picker;
    const bool folder = mode == L"folder" || mode == L"legacy-folder";
    if (!AwaitPicker(log, desktop, child, executable, branch, !folder, source, picker)) return;
    Handle helper(OpenProcess(SYNCHRONIZE, FALSE, picker.process));
    const bool driven = cancelled ? SendPicker(picker, 0, WM_KEYDOWN, VK_ESCAPE) : SelectThroughPicker(picker, text, folder);
    if (!log.Check(driven, branch + ": drive only verified Pulse controls")) return;
    ExpectedResult(log, desktop, child, branch, mode, cancelled, expected);
    watcher.Stop();
    log.Check(helper.get() && WaitForSingleObject(helper.get(), 5000) == WAIT_OBJECT_0,
        branch + ": owned picker helper exits after completing modal result");
}

void RunOpaqueFallback(TestLog& log, HDESK desktop, const std::wstring& desktop_name, const std::wstring& executable,
    const std::wstring& fixture, const std::filesystem::path& root) {
    const std::string branch = "opaque-open-fallback";
    ChildProcess child(log, branch);
    NativeDialogTakeover watcher(Appearance(), desktop, desktop_name);
    if (!log.Check(child.Start(fixture, {L"opaque-open", root.wstring()}, desktop_name), branch + ": launch own native fixture with non-pattern filter labels")) return;
    SourceWindow source;
    bool exited_early = false;
    const bool appeared = Until([&] {
        child.Drain();
        if (!child.Running()) { exited_early = true; return true; }
        source = FindSource(desktop, child.id());
        return source.window != nullptr;
    }, kAttachTimeout);
    if (!log.Check(appeared && !exited_early, branch + ": original native dialog appears on private desktop")) {
        Diagnostics(log, desktop, child, branch + ": native-startup-failure");
        return;
    }
    bool unexpected_picker = false, detector_accepted = false, filter_rejected = false;
    std::wstring reason;
    Until([&] {
        child.Drain();
        if (!child.Running() || !SourceMatches(source)) return true;
        unexpected_picker = FindPicker(desktop, source, executable).window != nullptr;
        if (unexpected_picker) return true;
        NativeFileDialog inspected;
        detector_accepted = pulse::app::InspectNativeFileDialog(source.window, inspected, reason);
        filter_rejected = !detector_accepted && reason.find(L"文件类型过滤器") != std::wstring::npos;
        return detector_accepted || filter_rejected;
    }, kAttachTimeout);
    log.Check(filter_rejected, branch + ": production detector explicitly rejects opaque filter metadata");
    log.Detail(branch + ": production detector reason=" + Utf8(reason));
    const ULONGLONG deadline = GetTickCount64() + 5000;
    bool interrupted = false;
    const bool observed = Until([&] {
        child.Drain();
        if (!child.Running() || !SourceMatches(source) || !IsWindowVisible(source.window) || !IsWindowEnabled(source.window)) {
            interrupted = true;
            return true;
        }
        if (FindPicker(desktop, source, executable).window) { unexpected_picker = true; return true; }
        return GetTickCount64() >= deadline;
    }, 6000);
    const bool preserved = observed && !interrupted && !unexpected_picker;
    log.Check(preserved, branch + ": active watcher leaves native visible/enabled without a Pulse popup throughout five-second observation");
    if (!filter_rejected || !preserved) Diagnostics(log, desktop, child, branch + ": unsupported-filter-safety-regression");
    watcher.Stop();
    ExpectedResult(log, desktop, child, branch + "-fixture-timer-cleanup", L"opaque-open", true, {});
}

bool CancelOwnNative(TestLog& log, const SourceWindow& source, const std::string& branch) {
    if (!SourceMatches(source)) return log.Check(false, branch + ": original fixture identity remains intact before production cancel");
    NativeFileDialog dialog;
    std::wstring reason;
    const bool inspected = pulse::app::InspectNativeFileDialog(source.window, dialog, reason);
    const bool cancelled = inspected && pulse::app::CancelNativeDialog(dialog, reason);
    log.Check(cancelled, branch + ": production CancelNativeDialog cancels only the own fixture");
    if (!cancelled) log.Detail(branch + ": cancel refused: " + Utf8(reason));
    return cancelled;
}

void RunStopRestart(TestLog& log, HDESK desktop, const std::wstring& desktop_name, const std::wstring& executable,
    const std::wstring& fixture, const std::filesystem::path& root) {
    const std::string branch = "stop-restart";
    ChildProcess child(log, branch);
    auto watcher = std::make_unique<NativeDialogTakeover>(Appearance(), desktop, desktop_name);
    if (!log.Check(child.Start(fixture, {L"open", root.wstring()}, desktop_name), branch + ": launch own fixture")) return;
    SourceWindow source;
    PickerWindow picker;
    if (!AwaitPicker(log, desktop, child, executable, branch, true, source, picker)) return;
    Handle helper(OpenProcess(SYNCHRONIZE, FALSE, picker.process));
    watcher->Stop();
    watcher.reset();
    const bool restored = Until([&] {
        return child.Running() && SourceMatches(source) && IsWindowVisible(source.window) && IsWindowEnabled(source.window) &&
            !PickerMatches(picker) && helper.get() && WaitForSingleObject(helper.get(), 0) == WAIT_OBJECT_0;
    }, kResultTimeout);
    log.Check(restored, branch + ": Stop/destruction closes picker but leaves original host alive, visible and enabled");
    if (!restored) { Diagnostics(log, desktop, child, branch + ": owner-restore-timeout"); return; }
    watcher = std::make_unique<NativeDialogTakeover>(Appearance(), desktop, desktop_name);
    PickerWindow replacement;
    if (!AwaitPicker(log, desktop, child, executable, branch + "-reenable", true, source, replacement)) return;
    log.Check(CompareFileTime(&picker.created, &replacement.created) != 0 || picker.process != replacement.process,
        branch + ": reenabling scans and takes over the same native dialog with a new helper");
    Handle replacement_helper(OpenProcess(SYNCHRONIZE, FALSE, replacement.process));
    watcher->Stop();
    watcher.reset();
    const bool restored_again = Until([&] { return child.Running() && SourceMatches(source) && IsWindowVisible(source.window) &&
        IsWindowEnabled(source.window) && !PickerMatches(replacement) && replacement_helper.get() &&
        WaitForSingleObject(replacement_helper.get(), 0) == WAIT_OBJECT_0; }, kResultTimeout);
    log.Check(restored_again, branch + ": second Stop restores native owner without forwarding selection or cancel");
    if (restored_again && CancelOwnNative(log, source, branch)) ExpectedResult(log, desktop, child, branch, L"open", true, {});
}

void RunParentDeath(TestLog& log, HDESK desktop, const std::wstring& desktop_name, const std::wstring& executable,
    const std::wstring& fixture, const std::filesystem::path& root) {
    const std::string branch = "parent-process-death";
    ChildProcess child(log, branch + "-fixture");
    ChildProcess parent(log, branch + "-own-test-host");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle ready(CreateEventW(&security, TRUE, FALSE, nullptr));
    if (!log.Check(ready.get() && parent.Start(executable,
        {L"--native-dialog-test-host", desktop_name, std::to_wstring(reinterpret_cast<uintptr_t>(ready.get()))},
        desktop_name, {ready.get()}), branch + ": launch exclusively owned watcher test-host")) return;
    const bool started = Until([&] { parent.Drain(); return !parent.Running() || WaitForSingleObject(ready.get(), 0) == WAIT_OBJECT_0; }, kAttachTimeout);
    if (!log.Check(started && parent.Running(), branch + ": isolated watcher test-host reports startup")) {
        log.Detail("test-host stdout=" + parent.bytes());
        return;
    }
    if (!log.Check(child.Start(fixture, {L"open", root.wstring()}, desktop_name), branch + ": launch own native fixture")) return;
    SourceWindow source;
    PickerWindow picker;
    if (!AwaitPicker(log, desktop, child, executable, branch, true, source, picker)) return;
    Handle helper(OpenProcess(SYNCHRONIZE, FALSE, picker.process));
    if (!log.Check(parent.Crash(), branch + ": simulate abrupt death only through the test-host handle returned by CreateProcess")) return;
    const bool restored = Until([&] {
        return child.Running() && SourceMatches(source) && IsWindowVisible(source.window) && IsWindowEnabled(source.window) &&
            !PickerMatches(picker) && helper.get() && WaitForSingleObject(helper.get(), 0) == WAIT_OBJECT_0;
    }, kResultTimeout);
    log.Check(restored, branch + ": helper independently restores original native owner and exits after real parent death");
    if (!restored) { Diagnostics(log, desktop, child, branch + ": parent-death-owner-restore-timeout"); return; }
    NativeDialogTakeover watcher(Appearance(), desktop, desktop_name);
    PickerWindow replacement;
    if (!AwaitPicker(log, desktop, child, executable, branch + "-reenable", true, source, replacement)) return;
    Handle replacement_helper(OpenProcess(SYNCHRONIZE, FALSE, replacement.process));
    if (!log.Check(SendPicker(replacement, 0, WM_KEYDOWN, VK_ESCAPE), branch + ": replacement picker cancels through production path")) return;
    ExpectedResult(log, desktop, child, branch + "-reenable", L"open", true, {});
    watcher.Stop();
    log.Check(replacement_helper.get() && WaitForSingleObject(replacement_helper.get(), 5000) == WAIT_OBJECT_0,
        branch + ": replacement helper exits after native cancellation");
}

bool WriteFixture(const std::filesystem::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    constexpr char content[] = "fixture";
    DWORD written = 0;
    return WriteFile(file.get(), content, static_cast<DWORD>(sizeof(content) - 1), &written, nullptr) && written == sizeof(content) - 1;
}

void RunIsolated(TestLog& log, HDESK desktop, const std::wstring& desktop_name) {
    if (!log.Check(SetThreadDesktop(desktop) != FALSE, "test worker attaches private desktop before COM/UI initialization")) return;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    if (!log.Check(SUCCEEDED(initialized), "test worker initializes COM for production native detector")) return;
    struct ComScope { ~ComScope() { CoUninitialize(); } } com;
    const auto executable = ExecutablePath();
    const auto fixture = (std::filesystem::path(executable).parent_path() / L"pulse_native_dialog_fixture.exe").wstring();
    const DWORD attributes = GetFileAttributesW(fixture.c_str());
    if (!log.Check(!executable.empty() && attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY),
        "native fixture executable exists beside tested Pulse binary")) return;
    const auto root = std::filesystem::temp_directory_path() /
        (L"PulseNativeTakeover-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!log.Check(std::filesystem::create_directory(root), "exclusively create temporary filesystem fixture")) return;
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    const auto nested = root / L"nested";
    const auto first = root / L"alpha.txt";
    const auto second = root / L"two words 中文.txt";
    const auto saved = root / L"new 保存.txt";
    if (!log.Check(std::filesystem::create_directory(nested) && WriteFixture(first) && WriteFixture(second),
        "create only disposable fixture directories and files")) return;
    const std::wstring names = L"\"" + first.wstring() + L"\" \"" + second.wstring() + L"\"";
    RunRound(log, desktop, desktop_name, executable, fixture, root, L"open", names, {first.wstring(), second.wstring()});
    log.Check(!std::filesystem::exists(saved), "save candidate does not exist before native save test");
    RunRound(log, desktop, desktop_name, executable, fixture, root, L"save", saved.wstring(), {saved.wstring()});
    log.Check(!std::filesystem::exists(saved), "native save returns selected path without creating or writing candidate");
    RunRound(log, desktop, desktop_name, executable, fixture, root, L"folder", nested.wstring(), {nested.wstring()});
    RunRound(log, desktop, desktop_name, executable, fixture, root, L"legacy-folder", nested.wstring(), {nested.wstring()});
    RunRound(log, desktop, desktop_name, executable, fixture, root, L"open", L"", {}, true);
    RunOpaqueFallback(log, desktop, desktop_name, executable, fixture, root);
    RunStopRestart(log, desktop, desktop_name, executable, fixture, root);
    RunParentDeath(log, desktop, desktop_name, executable, fixture, root);
    log.Check(std::filesystem::file_size(first) == 7 && std::filesystem::file_size(second) == 7 && !std::filesystem::exists(saved),
        "all takeover paths preserve original fixture files and never create save candidate");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    log.Check(!error && !std::filesystem::exists(root), "temporary filesystem fixture is removed after child process cleanup");
}
}

int RunNativeDialogTakeoverTest(const wchar_t* output) {
    if (!output || !output[0]) return 1;
    TestLog log(output);
    if (!log.stream) return 1;
    const std::wstring desktop_name = std::wstring(kDesktopPrefix) + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(GetTickCount64());
    constexpr DWORD access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW |
        DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL;
    const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, access, nullptr);
    if (!log.Check(desktop != nullptr, "create unique private desktop without SwitchDesktop or user-window interaction")) return 1;
    try {
        std::thread worker([&] {
            try { RunIsolated(log, desktop, desktop_name); }
            catch (const std::exception& error) { log.Check(false, "isolated test exception: " + std::string(error.what())); }
            catch (...) { log.Check(false, "isolated test exception: unknown"); }
        });
        worker.join();
    } catch (const std::exception& error) {
        log.Check(false, "test worker startup exception: " + std::string(error.what()));
    }
    log.Check(Until([&] { return CloseDesktop(desktop) != FALSE; }, 5000),
        "private desktop is closed after watcher, fixture, and helper cleanup");
    log.stream << "SUMMARY failures=" << log.failures << '\n';
    return log.failures ? 1 : 0;
}

int RunNativeDialogTakeoverTestHost(const wchar_t* desktop_name, HANDLE ready_event) {
    if (!desktop_name || !std::wstring_view(desktop_name).starts_with(kDesktopPrefix) || !ready_event) return 2;
    constexpr DWORD access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW |
        DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL;
    const HDESK desktop = OpenDesktopW(desktop_name, 0, FALSE, access);
    if (!desktop) return 3;
    if (!SetThreadDesktop(desktop)) { CloseDesktop(desktop); return 3; }
    NativeDialogTakeover watcher(Appearance(), desktop, desktop_name);
    if (!SetEvent(ready_event)) { watcher.Stop(); return 4; }
    CloseHandle(ready_event);
    Sleep(INFINITE);
    return 0;
}
