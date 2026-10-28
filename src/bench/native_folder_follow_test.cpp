#include "../app/native_folder_follow.h"

#include <atomic>
#include <cstdio>
#include <memory>

namespace {

int failures = 0;

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    if (!passed) ++failures;
}

void Pump(DWORD milliseconds) {
    const ULONGLONG until = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    } while (GetTickCount64() < until);
}

HWND Foreground(void* context) {
    return static_cast<std::atomic<HWND>*>(context)->load();
}

struct Windows {
    HWND source = nullptr, picker = nullptr, other = nullptr;
    Windows() {
        WNDCLASSW spec{};
        spec.hInstance = GetModuleHandleW(nullptr);
        spec.lpfnWndProc = DefWindowProcW;
        spec.lpszClassName = pulse::edition::kWindowClass;
        RegisterClassW(&spec);
        source = CreateWindowExW(0, spec.lpszClassName, L"Isolated folder source", 0,
            0, 0, 100, 100, nullptr, nullptr, spec.hInstance, nullptr);
        picker = CreateWindowExW(0, L"STATIC", L"Isolated picker", 0,
            0, 0, 100, 100, nullptr, nullptr, spec.hInstance, nullptr);
        other = CreateWindowExW(0, L"STATIC", L"Unrelated window", 0,
            0, 0, 100, 100, nullptr, nullptr, spec.hInstance, nullptr);
    }
    ~Windows() {
        if (source) DestroyWindow(source);
        if (picker) DestroyWindow(picker);
        if (other) DestroyWindow(other);
    }
};

struct StalledSource {
    PROCESS_INFORMATION process{};
    HANDLE stopped = nullptr;
    HWND window = nullptr;
    bool Start() {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        if (!length || length == ARRAYSIZE(executable)) return false;
        const auto name = L"Local\\PulseFolderFollowTest." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(GetTickCount64());
        stopped = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        if (!stopped) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(stopped); stopped = nullptr; return false; }
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --stalled-source \"" + name + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &process)) return false;
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(process.hProcess, &created, &exited, &kernel, &user);
        std::printf("[INFO] owned stalled-source pid=%lu created=%lu:%lu\n",
            process.dwProcessId, created.dwHighDateTime, created.dwLowDateTime);
        const ULONGLONG deadline = GetTickCount64() + 2000;
        do {
            EnumWindows([](HWND candidate, LPARAM context) -> BOOL {
                auto& fixture = *reinterpret_cast<StalledSource*>(context);
                DWORD pid = 0;
                GetWindowThreadProcessId(candidate, &pid);
                if (pid == fixture.process.dwProcessId && GetPropW(candidate, pulse::app::kFolderSourceMarker)) {
                    fixture.window = candidate;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            if (window) return true;
            if (WaitForSingleObject(process.hProcess, 10) != WAIT_TIMEOUT) return false;
        } while (GetTickCount64() < deadline);
        return false;
    }
    ~StalledSource() {
        if (stopped) SetEvent(stopped);
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 1000);
            }
            DWORD code = 1;
            Check(WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(process.hProcess, &code) && code == 0,
                "owned stalled-source process exits normally and is reaped");
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
        if (stopped) CloseHandle(stopped);
    }
};

void FollowWithoutListary() {
    using pulse::app::PublishFolderSource;
    Windows windows;
    Check(windows.source && windows.picker && windows.other, "isolated hidden source and picker windows create");
    if (!windows.source || !windows.picker || !windows.other) return;
    std::atomic<HWND> foreground{windows.picker};
    pulse::app::NativeFolderFollower follow(GetCurrentProcess(), GetCurrentProcessId(), Foreground, &foreground);
    follow.SetPicker(windows.picker);
    Check(follow.Start(), "native follower starts without Listary or a search plugin");

    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\initial", true);
    Pump(300);
    Check(follow.Poll().empty(), "background Pulse directory does not replace the picker's directory");
    foreground = windows.source;
    Pump(300);
    Check(follow.Poll().empty(), "source activation never navigates a background picker");
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll() == L"C:\\isolated-fixture\\initial", "returning to this picker delivers the visited folder");
    Pump(300);
    Check(follow.Poll().empty(), "one-shot delivery preserves subsequent manual picker navigation");
    foreground = windows.source;
    Pump(300);
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll() == L"C:\\isolated-fixture\\initial",
        "after manual picker navigation, reactivating the same source follows its unchanged folder once");
    Check(follow.Poll().empty(), "one source activation cannot repeatedly replace manual navigation");

    foreground = windows.source;
    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\next", true);
    Pump(300);
    PublishFolderSource(windows.source, L"\\\\server\\share\\\u4e2d\u6587", true);
    Pump(300);
    foreground = windows.other;
    Check(follow.Poll().empty(), "unrelated foreground windows cannot consume or apply a source update");
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll() == L"\\\\server\\share\\\u4e2d\u6587", "latest Unicode UNC visit supersedes earlier visits without probing the share");

    foreground = windows.source;
    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\before-last-tab", true);
    Pump(300);
    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\last-tab", true);
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll() == L"C:\\isolated-fixture\\last-tab",
        "worker rereads the final source directory after returning to the picker");

    // Inject the WinEvent without actually activating hidden fixture windows.
    // The source visit finishes before the next 125 ms polling sample.
    foreground = windows.source;
    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\quick-switch", true);
    NotifyWinEvent(EVENT_SYSTEM_FOREGROUND, windows.source, OBJID_WINDOW, CHILDID_SELF);
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll() == L"C:\\isolated-fixture\\quick-switch",
        "a completed source activation is captured even when no polling interval observed it");
    NotifyWinEvent(EVENT_SYSTEM_FOREGROUND, windows.source, OBJID_WINDOW, CHILDID_SELF);
    Pump(300);
    Check(follow.Poll() == L"C:\\isolated-fixture\\quick-switch",
        "a brief reactivation of the same source follows once after manual picker navigation");
    Pump(300);
    Check(follow.Poll().empty(), "event-driven reactivation still preserves one-shot delivery");

    Windows second;
    PublishFolderSource(second.source, L"C:\\isolated-fixture\\second-window", true);
    foreground = second.source;
    Pump(300);
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll() == L"C:\\isolated-fixture\\second-window",
        "activating another authenticated Pulse window replaces the prior source");

    foreground = windows.source;
    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\queued", true);
    Pump(300);
    PublishFolderSource(windows.source, L"shell:Home", true);
    Pump(300);
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll().empty(), "virtual folder clears an earlier undelivered physical directory");
    foreground = windows.source;
    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\queued", true);
    Pump(300);
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll() == L"C:\\isolated-fixture\\queued", "physical directory can be followed again after a virtual page");

    foreground = windows.source;
    Pump(300);
    PublishFolderSource(windows.source, L"shell:Home", true);
    foreground = windows.picker;
    Pump(300);
    Check(follow.Poll().empty(), "returning to picker rereads a last-moment virtual tab and discards the queued path");

    foreground = windows.source;
    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\revoked", true);
    Pump(300);
    PublishFolderSource(windows.source, {}, false);
    foreground = windows.picker;
    Check(!GetPropW(windows.source, pulse::app::kFolderSourceMarker) &&
        !GetDlgItem(windows.source, pulse::app::kFolderSourceControl) && follow.Poll().empty(),
        "disabling integration revokes the marker, read endpoint and queued delivery immediately");

    PublishFolderSource(windows.other, L"C:\\isolated-fixture\\wrong-class", true);
    foreground = windows.other;
    Pump(300);
    foreground = windows.picker;
    Check(follow.Poll().empty(), "a marked unrelated window class cannot become a folder source");

    PublishFolderSource(windows.source, L"C:\\isolated-fixture\\destroyed", true);
    foreground = windows.source;
    Pump(300);
    DestroyWindow(windows.source);
    windows.source = nullptr;
    foreground = windows.picker;
    Check(follow.Poll().empty(), "destroyed source cannot deliver a stale queued directory");
}

void IdentityAndCancellation() {
    using pulse::app::NativeFolderFollower;
    Windows windows;
    std::atomic<HWND> foreground{windows.source};
    NativeFolderFollower wrong(GetCurrentProcess(), GetCurrentProcessId() + 1, Foreground, &foreground);
    Check(!wrong.Start(), "parent handle and PID mismatch rejects the follower before observation");

    StalledSource child;
    const bool child_started = child.Start();
    Check(child_started, "isolated child publishes a source without pumping messages");
    if (!child_started) return;
    foreground = child.window;
    {
        NativeFolderFollower foreign(GetCurrentProcess(), GetCurrentProcessId(), Foreground, &foreground);
        foreign.SetPicker(windows.picker);
        Check(foreign.Start(), "foreign-source rejection fixture starts");
        Pump(300);
        foreground = windows.picker;
        Check(foreign.Poll().empty(), "correct source class and marker from a different PID are rejected");
    }
    foreground = child.window;
    auto follow = std::make_unique<NativeFolderFollower>(child.process.hProcess, child.process.dwProcessId, Foreground, &foreground);
    follow->SetPicker(windows.picker);
    Check(follow->Start(), "timeout fixture starts its owned worker");
    // The child never dispatches cross-process WM_GETTEXT; the worker must time out.
    Sleep(150);
    foreground = windows.picker;
    ULONGLONG started = GetTickCount64();
    const auto value = follow->Poll();
    Check(value.empty() && GetTickCount64() - started < 50,
        "UI polling stays nonblocking while the source's UI thread is stalled");
    started = GetTickCount64();
    follow.reset();
    Check(GetTickCount64() - started < 500, "cancellation joins the worker within its bounded read timeout");
}

void PathValidation() {
    using pulse::app::IsFolderSourcePath;
    Check(IsFolderSourcePath(L"C:\\") && IsFolderSourcePath(L"\\\\host\\share") &&
        IsFolderSourcePath(L"C:\\isolated-fixture\\\u4e2d\u6587"), "physical drive and UNC source paths validate without filesystem access");
    bool rejected = true;
    for (const auto* value : {L"", L"relative", L"C:relative", L"\\root-relative", L"\\\\host",
            L"\\\\?\\C:\\device", L"\\\\.\\pipe\\x", L"shell:Home", L"C:\\bad\nline", L"C:\\name:stream",
            L"\\\\host\\share:stream", L"\u4e2d:\\fake"}) rejected = rejected && !IsFolderSourcePath(value);
    Check(rejected, "virtual, device, relative, control and alternate-stream paths are rejected");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--stalled-source") {
        HANDLE stopped = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!stopped) return 1;
        Windows windows;
        pulse::app::PublishFolderSource(windows.source, L"C:\\isolated-fixture\\stalled", true);
        const DWORD result = WaitForSingleObject(stopped, 10000);
        CloseHandle(stopped);
        return result == WAIT_OBJECT_0 ? 0 : 1;
    }
    PathValidation();
    FollowWithoutListary();
    IdentityAndCancellation();
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
