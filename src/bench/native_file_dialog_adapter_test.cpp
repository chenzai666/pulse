#include "../app/native_file_dialog_adapter.h"

#include <commdlg.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <thread>

namespace {

using namespace pulse::app;
using Microsoft::WRL::ComPtr;
int failures = 0;

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!passed) ++failures;
}

HWND OwnedDialog() {
    HWND result = nullptr;
    EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND window, LPARAM context) -> BOOL {
        DWORD process = 0;
        wchar_t name[128]{};
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(window) &&
            GetClassNameW(window, name, 128) && std::wstring_view(name) == L"#32770") {
            *reinterpret_cast<HWND*>(context) = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

void CloseOwnedDialog(HWND window) {
    DWORD process = 0;
    if (window && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId())
        PostMessageW(window, WM_CLOSE, 0, 0);
}

struct Driver {
    std::atomic<bool> finished = false;
    std::atomic<bool> passed = false;
    std::wstring reason;
    std::thread thread;
    bool ready = false;

    explicit Driver(std::function<bool(const NativeFileDialog&, std::wstring&)> action) {
        const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
        std::promise<bool> startup;
        auto initialized_driver = startup.get_future();
        thread = std::thread([this, desktop, action = std::move(action), startup = std::move(startup)]() mutable {
            if (!desktop || !SetThreadDesktop(desktop)) {
                reason = L"Driver 无法在 COM 初始化前切换到 fixture 私有桌面。";
                startup.set_value(false);
                return;
            }
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialized)) {
                reason = L"Driver COM 初始化失败。";
                startup.set_value(false);
                return;
            }
            startup.set_value(true);
            HWND window = nullptr;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
            bool attempted = false;
            while (!finished && std::chrono::steady_clock::now() < deadline) {
                window = OwnedDialog();
                NativeFileDialog inspected;
                if (window && InspectNativeFileDialog(window, inspected, reason)) {
                    std::printf("[STAGE] driver inspected native dialog; applying selection/cancel\n");
                    passed = action(inspected, reason);
                    attempted = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            if (attempted && passed) {
                while (!finished && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            if (!finished) CloseOwnedDialog(window ? window : OwnedDialog());
            CoUninitialize();
        });
        ready = initialized_driver.get();
    }

    ~Driver() { finished = true; if (thread.joinable()) thread.join(); }
    void Finish() {
        finished = true;
        if (thread.joinable()) thread.join();
        if (!passed) std::fwprintf(stderr, L"适配失败: %ls\n", reason.c_str());
    }
};

void RejectionCases() {
    NativeFileDialog dialog;
    std::wstring reason;
    Check(!InspectNativeFileDialog(nullptr, dialog, reason) && !reason.empty() && !dialog.window,
        "invalid HWND is rejected with a diagnostic");
    Check(!NativeDialogStillMatches(dialog), "empty snapshot cannot match a live dialog");
    Check(!CommitNativeDialogSelection(dialog, {L"C:\\fixture.txt"}, 0, reason),
        "empty snapshot cannot submit a path");
    Check(!CancelNativeDialog(dialog, reason), "empty snapshot cannot cancel a host window");
    Check(!CommitNativeDialogSelection(dialog, {L"C:\\fixture.txt"}, 0, reason, [] { return true; }) &&
        reason.find(L"取消") != std::wstring::npos, "cancelled operation stops before window inspection");
    const HWND ordinary = CreateWindowExW(WS_EX_DLGMODALFRAME, L"#32770", L"Save", WS_POPUP,
        0, 0, 300, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(ordinary != nullptr, "isolated ordinary dialog fixture was created");
    if (!ordinary) return;
    CreateWindowExW(0, L"Button", L"Save", WS_CHILD | WS_VISIBLE, 0, 0, 80, 24,
        ordinary, reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
    CreateWindowExW(0, L"Button", L"Cancel", WS_CHILD | WS_VISIBLE, 80, 0, 80, 24,
        ordinary, reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleW(nullptr), nullptr);
    Check(!InspectNativeFileDialog(ordinary, dialog, reason) && !dialog.window,
        "ordinary #32770 with Save title and standard buttons is not a file dialog");
    DestroyWindow(ordinary);
}

struct Fixture {
    std::filesystem::path directory;
    std::filesystem::path file;
    std::filesystem::path folder;

    Fixture() {
        wchar_t temporary[32768]{};
        GetTempPathW(32768, temporary);
        directory = std::filesystem::path(temporary) /
            (L"Pulse-native-adapter-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        folder = directory / L"owned directory";
        file = directory / L"first file.txt";
        std::filesystem::create_directories(folder);
        std::ofstream(file, std::ios::binary) << "owned fixture";
        std::ofstream(directory / L"second file.txt", std::ios::binary) << "second owned fixture";
    }

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

void ModernCase(const Fixture& fixture, NativeDialogKind kind, bool cancel) {
    std::printf("[CASE] modern mode=%u cancel=%u\n", static_cast<unsigned>(kind), static_cast<unsigned>(cancel));
    ComPtr<IFileDialog> native;
    const CLSID& type = kind == NativeDialogKind::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(type, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native)))) {
        Check(false, "system IFileDialog is available");
        return;
    }
    const bool folder = kind == NativeDialogKind::Folder;
    native->SetOptions(FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR |
        (folder ? FOS_PICKFOLDERS : (kind == NativeDialogKind::Open ? FOS_FILEMUSTEXIST : FOS_PATHMUSTEXIST)));
    const COMDLG_FILTERSPEC filters[] = {{L"Text (*.txt)", L"*.txt"}, {L"Logs (*.log)", L"*.log"}};
    if (!folder) { native->SetFileTypes(2, filters); native->SetFileTypeIndex(2); }
    ComPtr<IShellItem> initial;
    if (SUCCEEDED(SHCreateItemFromParsingName(fixture.directory.c_str(), nullptr, IID_PPV_ARGS(&initial))))
        native->SetFolder(initial.Get());
    const std::wstring expected = folder ? fixture.folder.wstring() :
        (kind == NativeDialogKind::Save ? (fixture.directory / L"new file.txt").wstring() : fixture.file.wstring());
    std::atomic<bool> guarded = false;
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        if (inspected.kind != kind || (!folder && (inspected.filters.size() != 2 || inspected.filter_index != 1))) return false;
        auto stale = inspected;
        ++stale.process_id;
        if (NativeDialogStillMatches(stale) || CommitNativeDialogSelection(stale, {expected}, 0, reason)) return false;
        if (CommitNativeDialogSelection(inspected, {L"relative.txt"}, 0, reason)) return false;
        if (CommitNativeDialogSelection(inspected, {expected}, 0, reason, [] { return true; })) return false;
        guarded = true;
        return cancel ? CancelNativeDialog(inspected, reason) : CommitNativeDialogSelection(inspected, {expected}, 0, reason);
    });
    if (!driver.ready) { driver.Finish(); Check(false, "driver joins fixture desktop before COM and modal dialog"); return; }
    const HRESULT shown = native->Show(nullptr);
    driver.Finish();
    Check(guarded && driver.passed, "system dialog checks identity, relative paths and cancellation before submission");
    if (cancel) { Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED), "native host receives cancellation"); return; }
    ComPtr<IShellItem> selected;
    PWSTR result = nullptr;
    const bool got_result = SUCCEEDED(shown) && SUCCEEDED(native->GetResult(&selected)) &&
        SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &result));
    Check(got_result && result && CompareStringOrdinal(result, -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL,
        "modern native host returns the exact selected file or directory");
    CoTaskMemFree(result);
    if (!folder) {
        UINT index = 0;
        Check(SUCCEEDED(native->GetFileTypeIndex(&index)) && index == 1, "native host retains the selected file type");
    }
    if (kind == NativeDialogKind::Save)
        Check(!std::filesystem::exists(expected), "save adapter does not create or overwrite a file");
}

void ClassicCase(const Fixture& fixture) {
    std::printf("[CASE] classic Explorer file dialog\n");
    wchar_t result[32768]{};
    const std::wstring initial = fixture.directory.wstring();
    OPENFILENAMEW spec{};
    spec.lStructSize = sizeof(spec);
    spec.lpstrFilter = L"Text (*.txt)\0*.txt\0Logs (*.log)\0*.log\0\0";
    spec.nFilterIndex = 2;
    spec.lpstrFile = result;
    spec.nMaxFile = 32768;
    spec.lpstrInitialDir = initial.c_str();
    spec.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        return inspected.kind == NativeDialogKind::Open &&
            CommitNativeDialogSelection(inspected, {fixture.file.wstring()}, 0, reason);
    });
    if (!driver.ready) { driver.Finish(); Check(false, "classic driver initializes on fixture desktop"); return; }
    const bool shown = GetOpenFileNameW(&spec) != FALSE;
    driver.Finish();
    Check(driver.passed && shown && std::wstring_view(result) == fixture.file.wstring() && spec.nFilterIndex == 1,
        "classic Explorer common dialog returns the path and changed native filter");
}

void ModernMultiselectCase(const Fixture& fixture) {
    std::printf("[CASE] modern multiselect\n");
    ComPtr<IFileOpenDialog> native;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native)))) {
        Check(false, "system multiselect dialog is available");
        return;
    }
    native->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_ALLOWMULTISELECT);
    const COMDLG_FILTERSPEC filter{L"Text (*.txt)", L"*.txt"};
    native->SetFileTypes(1, &filter);
    ComPtr<IShellItem> initial;
    if (SUCCEEDED(SHCreateItemFromParsingName(fixture.directory.c_str(), nullptr, IID_PPV_ARGS(&initial))))
        native->SetFolder(initial.Get());
    std::vector<std::wstring> expected{fixture.file.wstring(), (fixture.directory / L"second file.txt").wstring()};
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        if (!inspected.allow_multiselect) return false;
        if (CommitNativeDialogSelection(inspected,
            {expected.front(), (fixture.folder / L"other.txt").wstring()}, 0, reason)) return false;
        return CommitNativeDialogSelection(inspected, expected, 0, reason);
    });
    if (!driver.ready) { driver.Finish(); Check(false, "multiselect driver initializes on fixture desktop"); return; }
    const HRESULT shown = native->Show(nullptr);
    driver.Finish();
    ComPtr<IShellItemArray> results;
    DWORD count = 0;
    std::vector<std::wstring> actual;
    if (SUCCEEDED(shown) && SUCCEEDED(native->GetResults(&results)) && SUCCEEDED(results->GetCount(&count))) {
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (SUCCEEDED(results->GetItemAt(index, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                actual.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    Check(driver.passed && actual == expected, "same-directory quoted names return both files; cross-directory multiselect is rejected");
}

void LegacyFolderCase(const Fixture& fixture) {
    std::printf("[CASE] legacy writable folder dialog\n");
    BROWSEINFOW spec{};
    spec.lpszTitle = L"Pulse owned legacy folder fixture";
    spec.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        return inspected.kind == NativeDialogKind::Folder && inspected.legacy_folder &&
            CommitNativeDialogSelection(inspected, {fixture.folder.wstring()}, 0, reason);
    });
    if (!driver.ready) { driver.Finish(); Check(false, "legacy driver initializes on fixture desktop"); return; }
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&spec);
    driver.Finish();
    wchar_t result[MAX_PATH]{};
    Check(driver.passed && selected && SHGetPathFromIDListW(selected, result) &&
        std::wstring_view(result) == fixture.folder.wstring(), "legacy writable folder edit returns the requested directory");
    CoTaskMemFree(selected);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const bool interaction = argc > 1 && std::wstring_view(argv[1]) == L"--interaction";
    // Interaction tests open real system file dialogs; create a private desktop so
    // they work reliably in headless/CI environments without disturbing the user desktop.
    HDESK desktop = nullptr;
    if (interaction) {
        const auto desktop_name = L"PulseAdapterTest_" + std::to_wstring(GetCurrentProcessId());
        constexpr DWORD access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW |
            DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL;
        desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, access, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) {
            Check(false, "interaction fixture must attach a private desktop before COM");
            if (desktop) CloseDesktop(desktop);
            return 2;
        }
        Check(true, "interaction fixture uses a private desktop without SwitchDesktop");
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 2;
    RejectionCases();
    if (interaction) {
        Fixture fixture;
        ModernCase(fixture, NativeDialogKind::Open, false);
        ModernCase(fixture, NativeDialogKind::Save, false);
        ModernCase(fixture, NativeDialogKind::Folder, false);
        ModernCase(fixture, NativeDialogKind::Open, true);
        ModernMultiselectCase(fixture);
        ClassicCase(fixture);
        LegacyFolderCase(fixture);
    }
    CoUninitialize();
    if (desktop) CloseDesktop(desktop);
    return failures ? 1 : 0;
}
