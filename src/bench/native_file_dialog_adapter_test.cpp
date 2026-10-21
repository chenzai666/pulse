#include "../app/native_file_dialog_adapter.h"

#include <commdlg.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <thread>

namespace {

using namespace pulse::app;
using Microsoft::WRL::ComPtr;
int failures = 0;

void Diagnostic(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return;
    std::string utf8(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        utf8.data(), size, nullptr, nullptr) == size) {
        std::fprintf(stderr, "[DETAIL] %s\n", utf8.c_str());
        std::fflush(stderr);
    }
}

void PathDiagnostic(const wchar_t* label, std::wstring_view value) {
    Diagnostic(std::wstring(label) + L" length=" + std::to_wstring(value.size()) + L" value=\"" + std::wstring(value) + L"\"");
}

bool NormalizeFixtureDirectory(std::filesystem::path& directory) {
    const HANDLE handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    std::wstring path(32768, L'\0');
    const DWORD size = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (!size || size >= path.size()) return false;
    path.resize(size);
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    directory = path;
    return true;
}

class NativeHostObserver final : public IFileDialogEvents {
public:
    std::atomic<HWND> filename_window = nullptr;
    std::atomic<unsigned> file_ok_count = 0;
    std::atomic<unsigned> overwrite_count = 0;
    DWORD file_ok_delay = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IFileDialogEvents)) return E_NOINTERFACE;
        *object = static_cast<IFileDialogEvents*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override {
        ++file_ok_count;
        if (file_ok_delay) Sleep(file_ok_delay);
        PWSTR filename = nullptr;
        const HRESULT named = dialog->GetFileName(&filename);
        std::printf("[HOST] OnFileOk GetFileName HRESULT=0x%08lX\n", static_cast<unsigned long>(named));
        PathDiagnostic(L"host.model_filename", filename ? std::wstring_view(filename) : std::wstring_view{});
        CoTaskMemFree(filename);
        const HWND edit = filename_window.load();
        DWORD process = 0;
        wchar_t klass[32]{};
        if (edit && GetWindowThreadProcessId(edit, &process) == GetCurrentThreadId() && process == GetCurrentProcessId() &&
            GetClassNameW(edit, klass, 32) && std::wstring_view(klass) == L"Edit") {
            std::wstring value(32768, L'\0');
            value.resize(static_cast<size_t>(GetWindowTextW(edit, value.data(), static_cast<int>(value.size()))));
            PathDiagnostic(L"host.edit_filename", value);
        }
        return LogType(dialog, "OnFileOk");
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (response) *response = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override { return LogType(dialog, "OnTypeChange"); }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        ++overwrite_count;
        std::printf("[HOST] OnOverwrite count=%u\n", overwrite_count.load());
        if (response) *response = FDEOR_DEFAULT;
        return S_OK;
    }
private:
    static HRESULT LogType(IFileDialog* dialog, const char* stage) {
        UINT index = 0;
        const HRESULT result = dialog->GetFileTypeIndex(&index);
        std::printf("[HOST] %s filter HRESULT=0x%08lX index=%u\n", stage, static_cast<unsigned long>(result), index);
        return S_OK;
    }
    std::atomic<ULONG> references_ = 1;
};

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
                    PathDiagnostic(L"inspected.initial_path", inspected.initial_path);
                    PathDiagnostic(L"inspected.filename", inspected.filename);
                    std::printf("[DETAIL] inspected mode=%u filter_count=%zu selected_filter=%zu multiselect=%u\n",
                        static_cast<unsigned>(inspected.kind), inspected.filters.size(), inspected.filter_index,
                        static_cast<unsigned>(inspected.allow_multiselect));
                    passed = action(inspected, reason);
                    std::printf("[STAGE] native action returned accepted=%u\n", static_cast<unsigned>(passed.load()));
                    if (!reason.empty()) Diagnostic(reason);
                    attempted = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            if (attempted && passed) {
                while (!finished && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            if (!finished) {
                if (attempted && passed) {
                    passed = false;
                    reason = L"原生请求已发送，但对话框未在期限内完成；fixture 清理将取消窗口。";
                }
                if (!reason.empty()) Diagnostic(reason);
                CloseOwnedDialog(window ? window : OwnedDialog());
            }
            CoUninitialize();
        });
        ready = initialized_driver.get();
    }

    ~Driver() { finished = true; if (thread.joinable()) thread.join(); }
    void Finish() {
        finished = true;
        if (thread.joinable()) thread.join();
        if (!passed && !reason.empty()) Diagnostic(reason);
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
        std::filesystem::create_directories(directory);
        PathDiagnostic(L"fixture.raw_directory", directory.wstring());
        Check(NormalizeFixtureDirectory(directory), "fixture directory resolves to the actual long path before exact path assertions");
        PathDiagnostic(L"fixture.normalized_directory", directory.wstring());
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
    if (kind == NativeDialogKind::Save)
        Check(SUCCEEDED(native->SetFileName(L"original model.log")), "save starts with an existing filename model that must be replaced");
    ComPtr<IShellItem> initial;
    if (SUCCEEDED(SHCreateItemFromParsingName(fixture.directory.c_str(), nullptr, IID_PPV_ARGS(&initial))))
        native->SetFolder(initial.Get());
    const std::wstring expected = folder ? fixture.folder.wstring() :
        (kind == NativeDialogKind::Save ? (fixture.folder / L"new file.txt").wstring() : fixture.file.wstring());
    ComPtr<NativeHostObserver> observer;
    observer.Attach(new NativeHostObserver());
    DWORD cookie = 0;
    const HRESULT observed = native->Advise(observer.Get(), &cookie);
    Check(SUCCEEDED(observed), "observe native filename model and type changes without modifying them");
    if (FAILED(observed)) return;
    struct Unadvise {
        IFileDialog* dialog;
        DWORD cookie;
        ~Unadvise() { dialog->Unadvise(cookie); }
    } subscription{native.Get(), cookie};
    std::atomic<bool> guarded = false;
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        observer->filename_window = inspected.filename_window;
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
    std::printf("[RESULT] modern Show HRESULT=0x%08lX\n", static_cast<unsigned long>(shown));
    driver.Finish();
    Check(guarded && driver.passed, "system dialog checks identity, relative paths and cancellation before submission");
    if (cancel) { Check(shown == HRESULT_FROM_WIN32(ERROR_CANCELLED), "native host receives cancellation"); return; }
    ComPtr<IShellItem> selected;
    PWSTR result = nullptr;
    const bool got_result = SUCCEEDED(shown) && SUCCEEDED(native->GetResult(&selected)) &&
        SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &result));
    PathDiagnostic(L"modern.expected", expected);
    PathDiagnostic(L"modern.actual", result ? std::wstring_view(result) : std::wstring_view{});
    Check(got_result && result && CompareStringOrdinal(result, -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL,
        "modern native host returns the exact selected file or directory");
    CoTaskMemFree(result);
    if (!folder) {
        UINT index = 0;
        const HRESULT type_result = native->GetFileTypeIndex(&index);
        std::printf("[RESULT] native filter HRESULT=0x%08lX index=%u\n", static_cast<unsigned long>(type_result), index);
        Check(SUCCEEDED(type_result) && index == 1, "native host retains the selected file type");
    }
    if (kind == NativeDialogKind::Save) {
        Check(observer->file_ok_count == 1, "cross-directory save reaches native validation exactly once");
        Check(!std::filesystem::exists(expected), "save adapter does not create or overwrite a file");
    }
}

UINT_PTR CALLBACK ClassicSaveHook(HWND, UINT message, WPARAM, LPARAM data) {
    if (message == WM_NOTIFY && data) {
        const auto* notification = reinterpret_cast<const OFNOTIFYW*>(data);
        if (notification->hdr.code == CDN_FILEOK && notification->lpOFN && notification->lpOFN->lCustData) {
            auto* confirmations = reinterpret_cast<std::atomic<unsigned>*>(notification->lpOFN->lCustData);
            ++*confirmations;
        }
    }
    return 0;
}

void ClassicCase(const Fixture& fixture, bool save = false) {
    std::printf("[CASE] classic Explorer file dialog save=%u\n", static_cast<unsigned>(save));
    wchar_t result[32768]{};
    if (save) wcscpy_s(result, L"original model.log");
    const std::wstring expected = save ? (fixture.folder / L"classic cross directory.txt").wstring() : fixture.file.wstring();
    std::atomic<unsigned> confirmations = 0;
    const std::wstring initial = fixture.directory.wstring();
    OPENFILENAMEW spec{};
    spec.lStructSize = sizeof(spec);
    spec.lpstrFilter = L"Text (*.txt)\0*.txt\0Logs (*.log)\0*.log\0\0";
    spec.nFilterIndex = 2;
    spec.lpstrFile = result;
    spec.nMaxFile = 32768;
    spec.lpstrInitialDir = initial.c_str();
    spec.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
        (save ? OFN_ENABLEHOOK | OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save) { spec.lpfnHook = ClassicSaveHook; spec.lCustData = reinterpret_cast<LPARAM>(&confirmations); }
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        if (inspected.kind != (save ? NativeDialogKind::Save : NativeDialogKind::Open)) return false;
        const bool submitted = CommitNativeDialogSelection(inspected, {expected}, 0, reason);
        if (!submitted) {
            NativeFileDialog prepared;
            std::wstring prepared_reason;
            const bool read = InspectNativeFileDialog(inspected.window, prepared, prepared_reason);
            std::printf("[DETAIL] classic prepared read=%u kind_before=%u kind_after=%u filter_before=%zu filter_after=%zu requested_filter=0\n",
                static_cast<unsigned>(read), static_cast<unsigned>(inspected.kind), static_cast<unsigned>(prepared.kind),
                inspected.filter_index, prepared.filter_index);
            std::printf("[DETAIL] classic prepared hwnd_same=%u edit_same=%u confirm_same=%u enabled=%u filters_before=%zu filters_after=%zu\n",
                static_cast<unsigned>(inspected.window == prepared.window), static_cast<unsigned>(inspected.filename_window == prepared.filename_window),
                static_cast<unsigned>(inspected.confirm_window == prepared.confirm_window),
                static_cast<unsigned>(IsWindowEnabled(inspected.confirm_window)), inspected.filters.size(), prepared.filters.size());
            PathDiagnostic(L"classic.requested_filename", expected);
            PathDiagnostic(L"classic.prepared_filename", prepared.filename);
            Diagnostic(prepared_reason);
        }
        return submitted;
    });
    if (!driver.ready) { driver.Finish(); Check(false, "classic driver initializes on fixture desktop"); return; }
    const bool shown = (save ? GetSaveFileNameW(&spec) : GetOpenFileNameW(&spec)) != FALSE;
    driver.Finish();
    std::printf("[RESULT] classic shown=%u extended_error=%lu selected_filter=%lu\n",
        static_cast<unsigned>(shown), CommDlgExtendedError(), spec.nFilterIndex);
    PathDiagnostic(L"classic.expected", expected);
    PathDiagnostic(L"classic.actual", result);
    Check(driver.passed && shown && std::wstring_view(result) == expected && spec.nFilterIndex == 1,
        "classic Explorer common dialog returns the path and changed native filter");
    if (save) {
        Check(confirmations == 1, "classic cross-directory save receives only one native file confirmation");
        Check(!std::filesystem::exists(expected), "classic save adapter does not create or overwrite the selected file");
    }
}

constexpr UINT kActivateFixtureWindow = WM_APP + 49;

LRESULT CALLBACK InactiveSaveHost(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kActivateFixtureWindow) {
        SetActiveWindow(window);
        return GetActiveWindow() == window;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void InactiveSaveCase(const Fixture& fixture) {
    std::printf("[CASE] inactive modern save sends one host notification\n");
    constexpr wchar_t host_class[] = L"PulseInactiveNativeSaveFixture";
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = InactiveSaveHost;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = host_class;
    if (!RegisterClassW(&window_class)) { Check(false, "register isolated inactive-save host"); return; }
    const HWND host = CreateWindowExW(WS_EX_TOOLWINDOW, host_class, L"Owned inactive-save fixture",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 320, 200, nullptr, nullptr, window_class.hInstance, nullptr);
    struct Cleanup {
        HWND window;
        const wchar_t* name;
        HINSTANCE instance;
        ~Cleanup() { if (window) DestroyWindow(window); UnregisterClassW(name, instance); }
    } cleanup{host, host_class, window_class.hInstance};
    if (!host) { Check(false, "create isolated inactive-save host"); return; }
    ComPtr<IFileSaveDialog> native;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native)))) {
        Check(false, "create inactive system save dialog"); return;
    }
    const COMDLG_FILTERSPEC filter{L"Text (*.txt)", L"*.txt"};
    native->SetFileTypes(1, &filter);
    native->SetFileName(L"original.txt");
    ComPtr<IShellItem> initial;
    if (SUCCEEDED(SHCreateItemFromParsingName(fixture.directory.c_str(), nullptr, IID_PPV_ARGS(&initial))))
        native->SetFolder(initial.Get());
    const std::wstring expected = (fixture.folder / L"inactive new save.txt").wstring();
    ComPtr<NativeHostObserver> observer;
    observer.Attach(new NativeHostObserver());
    observer->file_ok_delay = 300;
    DWORD cookie = 0;
    if (FAILED(native->Advise(observer.Get(), &cookie))) { Check(false, "observe inactive-save host notification"); return; }
    std::atomic<bool> concealed = false, inactive = false;
    unsigned submissions = 0;
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        observer->filename_window = inspected.filename_window;
        ShowWindowAsync(inspected.window, SW_HIDE);
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (IsWindowVisible(inspected.window) && GetTickCount64() < deadline) Sleep(10);
        NativeFileDialog early;
        concealed = !IsWindowVisible(inspected.window) && CanConcealNativeFileDialog(inspected.window, early, reason) &&
            early.window == inspected.window && early.process_id == inspected.process_id &&
            early.window_thread_id == inspected.window_thread_id && early.filename_window == inspected.filename_window &&
            early.confirm_window == inspected.confirm_window;
        ShowWindowAsync(inspected.window, SW_SHOWNOACTIVATE);
        while (!IsWindowVisible(inspected.window) && GetTickCount64() < deadline) Sleep(10);
        DWORD_PTR active = 0;
        inactive = SendMessageTimeoutW(host, kActivateFixtureWindow, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &active) && active != 0;
        if (!concealed || !inactive) return false;
        ++submissions;
        return CommitNativeDialogSelection(inspected, {expected}, 0, reason);
    });
    if (!driver.ready) { driver.Finish(); native->Unadvise(cookie); Check(false, "inactive-save driver initializes on fixture desktop"); return; }
    const HRESULT shown = native->Show(nullptr);
    driver.Finish();
    native->Unadvise(cookie);
    Check(concealed, "lightweight identity/structure precheck recognizes a hidden top-level native dialog");
    Check(inactive, "save native dialog is inactive before submitting its single confirmation");
    ComPtr<IShellItem> selected;
    PWSTR path = nullptr;
    const bool got_result = SUCCEEDED(shown) && SUCCEEDED(native->GetResult(&selected)) &&
        SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path));
    Check(driver.passed && submissions == 1 && observer->file_ok_count == 1 && got_result && path &&
        CompareStringOrdinal(path, -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL,
        "one inactive-save submission completes once after delayed native validation and returns the exact path");
    CoTaskMemFree(path);
    Check(!std::filesystem::exists(expected), "inactive-save adapter never creates the selected file");
}

bool DeclineOwnedOverwritePrompt(const NativeFileDialog& source, HWND prompt) {
    const auto matches = [&] {
        DWORD process = 0;
        return IsWindow(prompt) && GetWindow(prompt, GW_OWNER) == source.window &&
            GetWindowThreadProcessId(prompt, &process) == source.window_thread_id && process == source.process_id &&
            process == GetCurrentProcessId() && IsWindowVisible(prompt) && !IsWindowEnabled(source.window);
    };
    if (!matches()) return false;
    ComPtr<IUIAutomation2> timed;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&timed))) ||
        FAILED(timed->put_ConnectionTimeout(500)) || FAILED(timed->put_TransactionTimeout(500)) ||
        FAILED(timed.As(&automation))) return false;
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> buttons;
    VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ButtonControlTypeId;
    if (FAILED(automation->ElementFromHandle(prompt, &root)) ||
        FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)) ||
        FAILED(root->FindAll(TreeScope_Subtree, condition.Get(), &buttons))) return false;
    int count = 0;
    if (!buttons || FAILED(buttons->get_Length(&count)) || count > 32) return false;
    ComPtr<IUIAutomationElement> negative;
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> button;
        if (FAILED(buttons->GetElement(index, &button)) || !button) continue;
        BSTR raw_name = nullptr, raw_id = nullptr;
        button->get_CurrentName(&raw_name);
        button->get_CurrentAutomationId(&raw_id);
        const std::wstring name = raw_name ? std::wstring(raw_name, SysStringLen(raw_name)) : std::wstring();
        const std::wstring id = raw_id ? std::wstring(raw_id, SysStringLen(raw_id)) : std::wstring();
        SysFreeString(raw_name); SysFreeString(raw_id);
        Diagnostic(L"overwrite.uia_name=" + name + L" automation_id=" + id);
        int process = 0, control = 0;
        BOOL enabled = FALSE, offscreen = TRUE;
        if (FAILED(button->get_CurrentProcessId(&process)) || process != static_cast<int>(source.process_id) ||
            FAILED(button->get_CurrentControlType(&control)) || control != UIA_ButtonControlTypeId ||
            FAILED(button->get_CurrentIsEnabled(&enabled)) || !enabled ||
            FAILED(button->get_CurrentIsOffscreen(&offscreen)) || offscreen) continue;
        if (name != L"No" && name != L"&No" && name != L"否" && name != L"否(&N)") continue;
        if (negative) return false;
        negative = button;
    }
    if (!negative) return false;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_RawViewWalker(&walker))) return false;
    auto ancestor = negative;
    bool below_prompt = false;
    for (unsigned depth = 0; ancestor && depth < 32; ++depth) {
        BOOL same = FALSE;
        if (FAILED(automation->CompareElements(ancestor.Get(), root.Get(), &same))) return false;
        if (same) { below_prompt = true; break; }
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(ancestor.Get(), &parent))) return false;
        ancestor = parent;
    }
    UIA_HWND native = nullptr;
    if (FAILED(negative->get_CurrentNativeWindowHandle(&native))) return false;
    if (native) {
        DWORD process = 0;
        const HWND window = reinterpret_cast<HWND>(native);
        if (GetAncestor(window, GA_ROOT) != prompt ||
            GetWindowThreadProcessId(window, &process) != source.window_thread_id || process != source.process_id) return false;
    }
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (!below_prompt || !matches() ||
        FAILED(negative->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke)))) return false;
    const HRESULT invoked = invoke->Invoke();
    std::printf("[DETAIL] overwrite explicit-No UIA invoke=0x%08lX\n", static_cast<unsigned long>(invoked));
    return SUCCEEDED(invoked);
}

void ExistingFileSaveCase(const std::filesystem::path& directory) {
    std::printf("[CASE] cross-directory save preserves native overwrite confirmation\n");
    const auto expected = directory / L"owned directory" / L"existing save.txt";
    constexpr char original[] = "original owned content";
    std::ofstream(expected, std::ios::binary) << original;
    ComPtr<IFileSaveDialog> native;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&native)))) {
        Check(false, "create native overwrite-prompt fixture"); return;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    native->GetOptions(&options);
    native->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
        FOS_OVERWRITEPROMPT | FOS_NOTESTFILECREATE | FOS_DONTADDTORECENT);
    const COMDLG_FILTERSPEC filter{L"Text (*.txt)", L"*.txt"};
    native->SetFileTypes(1, &filter);
    native->SetFileName(L"original.txt");
    ComPtr<IShellItem> initial;
    if (SUCCEEDED(SHCreateItemFromParsingName(directory.c_str(), nullptr, IID_PPV_ARGS(&initial))))
        native->SetFolder(initial.Get());
    ComPtr<NativeHostObserver> observer;
    observer.Attach(new NativeHostObserver());
    DWORD cookie = 0;
    if (FAILED(native->Advise(observer.Get(), &cookie))) { Check(false, "observe native overwrite validation"); return; }
    std::atomic<bool> prompt_seen = false, declined = false;
    unsigned submissions = 0;
    Driver driver([&](const NativeFileDialog& inspected, std::wstring& reason) {
        observer->filename_window = inspected.filename_window;
        ++submissions;
        const bool submitted = CommitNativeDialogSelection(inspected, {expected.wstring()}, 0, reason);
        if (!submitted) return false;
        HWND prompt = nullptr;
        const ULONGLONG deadline = GetTickCount64() + 5000;
        do {
            const HWND popup = GetWindow(inspected.window, GW_ENABLEDPOPUP);
            DWORD process = 0;
            wchar_t klass[32]{};
            if (popup && popup != inspected.window && GetWindow(popup, GW_OWNER) == inspected.window &&
                GetWindowThreadProcessId(popup, &process) == inspected.window_thread_id && process == inspected.process_id &&
                GetClassNameW(popup, klass, 32) && std::wstring_view(klass) == L"#32770" &&
                IsWindowVisible(popup) && !IsWindowEnabled(inspected.window)) {
                prompt = popup;
                break;
            }
            Sleep(20);
        } while (GetTickCount64() < deadline);
        prompt_seen = prompt != nullptr;
        std::printf("[DETAIL] overwrite submitted=%u prompt=%llu overwrite_events=%u file_ok_events=%u\n",
            static_cast<unsigned>(submitted), static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(prompt)),
            observer->overwrite_count.load(), observer->file_ok_count.load());
        if (prompt) {
            EnumChildWindows(prompt, [](HWND control, LPARAM) -> BOOL {
                wchar_t klass[128]{};
                GetClassNameW(control, klass, 128);
                std::printf("[DETAIL] overwrite control id=%d visible=%u enabled=%u\n", GetDlgCtrlID(control),
                    static_cast<unsigned>(IsWindowVisible(control)), static_cast<unsigned>(IsWindowEnabled(control)));
                Diagnostic(std::wstring(L"overwrite.control_class=") + klass);
                return TRUE;
            }, 0);
            const auto verified_button = [&](int id) -> HWND {
                const HWND button = GetDlgItem(prompt, id);
                DWORD process = 0;
                wchar_t klass[32]{};
                return button && GetParent(button) == prompt && GetDlgCtrlID(button) == id &&
                    GetWindowThreadProcessId(button, &process) == inspected.window_thread_id && process == inspected.process_id &&
                    GetClassNameW(button, klass, 32) && std::wstring_view(klass) == L"Button" &&
                    IsWindowVisible(button) && IsWindowEnabled(button) ? button : nullptr;
            };
            const HWND no = verified_button(IDNO);
            const HWND yes = verified_button(IDYES);
            // A Yes/No prompt may ignore WM_CLOSE. Route only its verified
            // negative button notification; never invoke its default button.
            const bool negative_button = no && yes;
            const bool posted = negative_button
                ? PostMessageW(prompt, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), reinterpret_cast<LPARAM>(no)) != FALSE
                : DeclineOwnedOverwritePrompt(inspected, prompt);
            std::printf("[DETAIL] overwrite negative_button=%u cancel-posted=%u\n",
                static_cast<unsigned>(negative_button), static_cast<unsigned>(posted));
            const ULONGLONG decline_deadline = GetTickCount64() + 2000;
            while (IsWindow(prompt) && GetTickCount64() < decline_deadline) Sleep(20);
            declined = !IsWindow(prompt) && IsWindow(inspected.window) && IsWindowEnabled(inspected.window);
            std::printf("[DETAIL] overwrite declined=%u source_enabled=%u\n", static_cast<unsigned>(declined.load()),
                static_cast<unsigned>(IsWindowEnabled(inspected.window)));
        }
        CloseOwnedDialog(inspected.window);
        return submitted && prompt_seen && declined;
    });
    if (!driver.ready) { driver.Finish(); native->Unadvise(cookie); Check(false, "overwrite driver initializes privately"); return; }
    const HRESULT shown = native->Show(nullptr);
    driver.Finish();
    native->Unadvise(cookie);
    Check(driver.passed && submissions == 1 && prompt_seen && observer->overwrite_count <= 1,
        "one cross-directory save exposes the host overwrite confirmation without repeated submission");
    Check(declined && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) && observer->file_ok_count <= 1,
        "declining the fixture overwrite leaves save pending until explicit cancellation without repeated confirmation");
    std::ifstream preserved(expected, std::ios::binary);
    const std::string actual(std::istreambuf_iterator<char>(preserved), {});
    Check(actual == original, "declined native overwrite preserves the existing fixture file byte for byte");
}

void ExistingFileSaveProcess(const Fixture& fixture) {
    struct OwnedHandle {
        HANDLE value = nullptr;
        ~OwnedHandle() { if (value) CloseHandle(value); }
    } read, write, process, thread;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&read.value, &write.value, &security, 0) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) {
        Check(false, "create bounded overwrite-test output pipe"); return;
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes_storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
        Check(false, "initialize overwrite-test handle isolation"); return;
    }
    struct AttributesScope {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributesScope() { DeleteProcThreadAttributeList(value); }
    } attribute_scope{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        &write.value, sizeof(write.value), nullptr, nullptr)) {
        Check(false, "restrict overwrite-test inheritance to its own output handle"); return;
    }
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) { Check(false, "locate overwrite-test executable"); return; }
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --overwrite-interaction \"" +
        fixture.directory.wstring() + L"\"";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write.value;
    startup.StartupInfo.hStdError = write.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &created)) {
        Check(false, "launch isolated overwrite-test process"); return;
    }
    process.value = created.hProcess;
    thread.value = created.hThread;
    CloseHandle(write.value); write.value = nullptr;
    std::printf("[DETAIL] overwrite child pid=%lu start_tick=%llu timeout_ms=25000\n", created.dwProcessId, GetTickCount64());
    const auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            char output[4096]; DWORD received = 0;
            if (!ReadFile(read.value, output, (std::min)(available, static_cast<DWORD>(sizeof(output))), &received, nullptr) || !received)
                break;
            std::fwrite(output, 1, received, stdout);
        }
    };
    const ULONGLONG deadline = GetTickCount64() + 25000;
    while (WaitForSingleObject(process.value, 20) == WAIT_TIMEOUT && GetTickCount64() < deadline) drain();
    const bool finished = WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0;
    if (!finished) {
        // This handle comes exclusively from our CreateProcess call. A stuck
        // nested system modal must not hang the remaining adapter cases.
        const bool terminated = TerminateProcess(process.value, ERROR_TIMEOUT) != FALSE;
        Check(terminated && WaitForSingleObject(process.value, 5000) == WAIT_OBJECT_0,
            "timed-out owned overwrite-test process is terminated and reaped");
    }
    drain();
    DWORD exit_code = STILL_ACTIVE;
    GetExitCodeProcess(process.value, &exit_code);
    std::printf("[DETAIL] overwrite child finished=%u exit_code=%lu\n", static_cast<unsigned>(finished), exit_code);
    Check(finished && exit_code == 0, "isolated overwrite confirmation and cancellation complete within their process deadline");
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
        std::vector<std::wstring> selected = expected;
        std::wstring short_directory(32768, L'\0');
        const DWORD size = GetShortPathNameW(fixture.directory.c_str(), short_directory.data(), static_cast<DWORD>(short_directory.size()));
        if (size && size < short_directory.size()) {
            short_directory.resize(size);
            PathDiagnostic(L"multiselect.input_directory_alias", short_directory);
            for (auto& path : selected)
                path = (std::filesystem::path(short_directory) / std::filesystem::path(path).filename()).wstring();
        }
        return CommitNativeDialogSelection(inspected, selected, 0, reason);
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
    std::printf("[RESULT] multiselect Show HRESULT=0x%08lX expected_count=%zu actual_count=%zu\n",
        static_cast<unsigned long>(shown), expected.size(), actual.size());
    for (const auto& path : expected) PathDiagnostic(L"multiselect.expected", path);
    for (const auto& path : actual) PathDiagnostic(L"multiselect.actual", path);
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
    const bool got_path = selected && SHGetPathFromIDListW(selected, result);
    PathDiagnostic(L"legacy.expected", fixture.folder.wstring());
    PathDiagnostic(L"legacy.actual", result);
    Check(driver.passed && got_path &&
        std::wstring_view(result) == fixture.folder.wstring(), "legacy writable folder edit returns the requested directory");
    CoTaskMemFree(selected);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const bool overwrite = argc == 3 && std::wstring_view(argv[1]) == L"--overwrite-interaction";
    const bool interaction = overwrite || (argc > 1 && std::wstring_view(argv[1]) == L"--interaction");
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
    if (overwrite) {
        ExistingFileSaveCase(std::filesystem::path(argv[2]));
    } else {
        RejectionCases();
    }
    if (interaction && !overwrite) {
        Fixture fixture;
        ModernCase(fixture, NativeDialogKind::Open, false);
        ModernCase(fixture, NativeDialogKind::Save, false);
        InactiveSaveCase(fixture);
        ExistingFileSaveProcess(fixture);
        ModernCase(fixture, NativeDialogKind::Folder, false);
        ModernCase(fixture, NativeDialogKind::Open, true);
        ModernMultiselectCase(fixture);
        ClassicCase(fixture);
        ClassicCase(fixture, true);
        LegacyFolderCase(fixture);
    }
    CoUninitialize();
    if (desktop) CloseDesktop(desktop);
    return failures ? 1 : 0;
}
