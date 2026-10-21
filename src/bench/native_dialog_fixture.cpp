#include "../common/windows_compat.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <wrl/client.h>

#include <cstdio>
#include <atomic>
#include <bit>
#include <fcntl.h>
#include <io.h>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;

enum class FixtureMode { Open, Save, Folder, LegacyFolder, OpaqueOpen };
constexpr DWORD kOpaqueCancelTimeout = 15000;

struct DialogResult {
    HRESULT error = E_FAIL;
    std::vector<std::wstring> paths;
    UINT filter_index = 0;
    bool fixture_cancel_posted = false;
    DWORD fixture_cancel_error = ERROR_SUCCESS;
    unsigned file_ok_count = 0;
    bool file_ok_concealed = false;
    bool transient_popup_observed = false;
};

struct ComScope {
    ~ComScope() { CoUninitialize(); }
};

struct WindowScope {
    HWND window = nullptr;
    ~WindowScope() { if (window) DestroyWindow(window); }
};

struct TaskFree {
    void operator()(void* value) const { CoTaskMemFree(value); }
};

bool ParseMode(std::wstring_view text, FixtureMode& mode) {
    if (text == L"open") mode = FixtureMode::Open;
    else if (text == L"save") mode = FixtureMode::Save;
    else if (text == L"folder") mode = FixtureMode::Folder;
    else if (text == L"legacy-folder") mode = FixtureMode::LegacyFolder;
    else if (text == L"opaque-open") mode = FixtureMode::OpaqueOpen;
    else return false;
    return true;
}

const char* ModeName(FixtureMode mode) {
    switch (mode) {
    case FixtureMode::Open: return "open";
    case FixtureMode::Save: return "save";
    case FixtureMode::Folder: return "folder";
    case FixtureMode::LegacyFolder: return "legacy-folder";
    case FixtureMode::OpaqueOpen: return "opaque-open";
    }
    return "unknown";
}

bool AppendJsonString(std::string& json, std::wstring_view text) {
    std::string utf8;
    if (!text.empty()) {
        if (text.size() > 32768) return false;
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (!size) return false;
        utf8.resize(static_cast<size_t>(size));
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr) != size) return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    json += '"';
    for (const unsigned char byte : utf8) {
        if (byte == '"' || byte == '\\') { json += '\\'; json += static_cast<char>(byte); }
        else if (byte < 32) {
            json += "\\u00";
            json += hex[byte >> 4];
            json += hex[byte & 15];
        } else json += static_cast<char>(byte);
    }
    json += '"';
    return true;
}

int EmitResult(const char* mode, DialogResult result, int error_exit = 1) {
    const bool cancelled = result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED);
    const bool accepted = SUCCEEDED(result.error);
    std::string json = "{\"status\":\"";
    json += accepted ? "accepted" : cancelled ? "cancelled" : "error";
    json += "\",\"mode\":\"";
    json += mode;
    json += "\",\"paths\":[";
    if (accepted) {
        for (size_t index = 0; index < result.paths.size(); ++index) {
            if (index) json += ',';
            if (!AppendJsonString(json, result.paths[index])) {
                DialogResult failed;
                failed.error = HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
                return EmitResult(mode, std::move(failed));
            }
        }
    }
    json += "],\"filter_index\":" + std::to_string(result.filter_index);
    if (std::string_view(mode) == "save") {
        json += ",\"file_ok_count\":" + std::to_string(result.file_ok_count);
        json += ",\"file_ok_concealed\":";
        json += result.file_ok_concealed ? "true" : "false";
        json += ",\"transient_popup_observed\":";
        json += result.transient_popup_observed ? "true" : "false";
    }
    if (std::string_view(mode) == "opaque-open") {
        json += ",\"fixture_cancel_posted\":";
        json += result.fixture_cancel_posted ? "true" : "false";
        json += ",\"fixture_cancel_error\":" + std::to_string(result.fixture_cancel_error);
    }
    json += ",\"hresult\":" + std::to_string(static_cast<unsigned long>(result.error)) + "}\n";
    if (std::fwrite(json.data(), 1, json.size(), stdout) != json.size() || std::fflush(stdout) != 0) return 3;
    return accepted || cancelled ? 0 : error_exit;
}

HRESULT ItemPath(IShellItem* item, std::wstring& path) {
    PWSTR raw = nullptr;
    const HRESULT result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    std::unique_ptr<wchar_t, TaskFree> owned(raw);
    if (FAILED(result)) return result;
    if (!raw || !raw[0]) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    path = raw;
    return S_OK;
}

class OpaqueCancellation {
public:
    OpaqueCancellation() = default;
    OpaqueCancellation(const OpaqueCancellation&) = delete;
    OpaqueCancellation& operator=(const OpaqueCancellation&) = delete;
    ~OpaqueCancellation() {
        Stop();
        if (worker_) CloseHandle(worker_);
        if (cancelled_) CloseHandle(cancelled_);
    }
    bool Start(HWND owner) {
        owner_ = owner;
        thread_id_ = GetCurrentThreadId();
        process_id_ = GetCurrentProcessId();
        desktop_ = GetThreadDesktop(thread_id_);
        cancelled_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!cancelled_ || !desktop_) return false;
        worker_ = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        return worker_ != nullptr;
    }
    void Stop() {
        if (cancelled_) SetEvent(cancelled_);
        if (worker_) WaitForSingleObject(worker_, INFINITE);
    }
    bool Posted() const { return posted_; }
    DWORD Error() const { return error_; }
private:
    bool Matches(HWND window) const {
        DWORD process = 0;
        wchar_t name[32]{};
        return window && GetWindowThreadProcessId(window, &process) == thread_id_ && process == process_id_ &&
            GetWindow(window, GW_OWNER) == owner_ && IsWindowVisible(window) && IsWindowEnabled(window) &&
            GetClassNameW(window, name, static_cast<int>(std::size(name))) && std::wstring_view(name) == L"#32770";
    }
    static DWORD WINAPI Run(void* raw) {
        auto& state = *static_cast<OpaqueCancellation*>(raw);
        if (!SetThreadDesktop(state.desktop_)) { state.error_ = GetLastError(); return 0; }
        const DWORD waited = WaitForSingleObject(state.cancelled_, kOpaqueCancelTimeout);
        if (waited == WAIT_OBJECT_0) return 0;
        if (waited != WAIT_TIMEOUT) { state.error_ = GetLastError(); return 0; }
        DWORD owner_process = 0;
        if (GetWindowThreadProcessId(state.owner_, &owner_process) != state.thread_id_ || owner_process != state.process_id_) {
            state.error_ = ERROR_INVALID_WINDOW_HANDLE;
            return 0;
        }
        EnumThreadWindows(state.thread_id_, [](HWND window, LPARAM data) -> BOOL {
            auto& context = *reinterpret_cast<OpaqueCancellation*>(data);
            if (!context.Matches(window) || WaitForSingleObject(context.cancelled_, 0) != WAIT_TIMEOUT) return TRUE;
            context.posted_ = PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
            context.error_ = context.posted_ ? ERROR_SUCCESS : GetLastError();
            return FALSE;
        }, reinterpret_cast<LPARAM>(&state));
        if (!state.posted_ && state.error_ == ERROR_SUCCESS) state.error_ = ERROR_NOT_FOUND;
        return 0;
    }
    HANDLE cancelled_ = nullptr, worker_ = nullptr;
    HDESK desktop_ = nullptr;
    HWND owner_ = nullptr;
    DWORD thread_id_ = 0, process_id_ = 0, error_ = ERROR_SUCCESS;
    bool posted_ = false;
};

class SaveObserver final : public IFileDialogEvents {
public:
    unsigned file_ok_count = 0;
    bool file_ok_concealed = false;
    bool transient_popup_observed = false;
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
        ComPtr<IOleWindow> native_window;
        HWND window = nullptr;
        const bool own_window = SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&native_window))) &&
            SUCCEEDED(native_window->GetWindow(&window)) && window;
        DWORD process = 0;
        if (own_window && GetWindowThreadProcessId(window, &process) == GetCurrentThreadId() &&
            process == GetCurrentProcessId()) {
            const bool was_enabled = IsWindowEnabled(window) != FALSE;
            const HWND progress = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Owned fixture save progress",
                WS_POPUP | WS_VISIBLE, 0, 0, 200, 60, window, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (progress) {
                EnableWindow(window, FALSE);
                transient_popup_observed = GetWindow(progress, GW_OWNER) == window &&
                    IsWindowVisible(progress) && !IsWindowEnabled(window);
            }
            Sleep(100);
            if (progress) {
                DestroyWindow(progress);
                EnableWindow(window, was_enabled ? TRUE : FALSE);
            }
        } else {
            Sleep(100);
        }
        Sleep(200);
        BYTE alpha = 255;
        DWORD flags = 0;
        file_ok_concealed = own_window &&
            GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) != 0 && alpha == 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (response) *response = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        if (response) *response = FDEOR_DEFAULT;
        return S_OK;
    }
private:
    std::atomic<ULONG> references_ = 1;
};

DialogResult ShowModern(HWND owner, FixtureMode mode, IShellItem* initial, const std::wstring& filename) {
    DialogResult result;
    ComPtr<IFileDialog> dialog;
    ComPtr<IFileOpenDialog> open;
    if (mode == FixtureMode::Save) {
        result.error = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    } else {
        result.error = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&open));
        if (SUCCEEDED(result.error)) result.error = open.As(&dialog);
    }
    if (FAILED(result.error)) return result;
    FILEOPENDIALOGOPTIONS options = 0;
    result.error = dialog->GetOptions(&options);
    if (FAILED(result.error)) return result;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT;
    if (mode == FixtureMode::Open || mode == FixtureMode::OpaqueOpen) options |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
    else if (mode == FixtureMode::Folder) options |= FOS_PICKFOLDERS;
    else options |= FOS_OVERWRITEPROMPT | FOS_NOTESTFILECREATE;
    result.error = dialog->SetOptions(options);
    if (FAILED(result.error)) return result;
    result.error = dialog->SetFolder(initial);
    if (FAILED(result.error)) return result;
    const wchar_t* title = mode == FixtureMode::OpaqueOpen ? L"Pulse native dialog fixture: opaque-open"
        : mode == FixtureMode::Open ? L"Pulse native dialog fixture: open"
        : mode == FixtureMode::Save ? L"Pulse native dialog fixture: save"
        : L"Pulse native dialog fixture: folder";
    result.error = dialog->SetTitle(title);
    if (FAILED(result.error)) return result;
    if (mode != FixtureMode::Folder) {
        const COMDLG_FILTERSPEC filters[] = {
            {mode == FixtureMode::OpaqueOpen ? L"Opaque all files" : L"All files (*.*)", L"*.*"},
            {mode == FixtureMode::OpaqueOpen ? L"Opaque text documents" : L"Text documents (*.txt)", L"*.txt"}};
        result.error = dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        if (FAILED(result.error)) return result;
        result.error = dialog->SetFileTypeIndex(1);
        if (FAILED(result.error)) return result;
        if (!filename.empty()) {
            result.error = dialog->SetFileName(filename.c_str());
            if (FAILED(result.error)) return result;
        }
    }
    OpaqueCancellation cancellation;
    if (mode == FixtureMode::OpaqueOpen) {
        if (!cancellation.Start(owner)) {
            const DWORD error = GetLastError();
            result.error = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            return result;
        }
    }
    ComPtr<SaveObserver> observer;
    DWORD observer_cookie = 0;
    if (mode == FixtureMode::Save) {
        observer.Attach(new SaveObserver());
        result.error = dialog->Advise(observer.Get(), &observer_cookie);
        if (FAILED(result.error)) return result;
    }
    result.error = dialog->Show(owner);
    if (observer) {
        result.file_ok_count = observer->file_ok_count;
        result.file_ok_concealed = observer->file_ok_concealed;
        result.transient_popup_observed = observer->transient_popup_observed;
        dialog->Unadvise(observer_cookie);
    }
    if (mode == FixtureMode::OpaqueOpen) {
        cancellation.Stop();
        result.fixture_cancel_posted = cancellation.Posted();
        result.fixture_cancel_error = cancellation.Error();
    }
    if (FAILED(result.error)) return result;
    if (mode == FixtureMode::Open || mode == FixtureMode::OpaqueOpen) {
        ComPtr<IShellItemArray> items;
        result.error = open->GetResults(&items);
        if (FAILED(result.error)) return result;
        DWORD count = 0;
        result.error = items->GetCount(&count);
        if (FAILED(result.error)) return result;
        if (!count || count > 1024) { result.error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return result; }
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            result.error = items->GetItemAt(index, &item);
            if (FAILED(result.error)) return result;
            std::wstring path;
            result.error = ItemPath(item.Get(), path);
            if (FAILED(result.error)) return result;
            result.paths.push_back(std::move(path));
        }
    } else {
        ComPtr<IShellItem> item;
        result.error = dialog->GetResult(&item);
        if (FAILED(result.error)) return result;
        std::wstring path;
        result.error = ItemPath(item.Get(), path);
        if (FAILED(result.error)) return result;
        result.paths.push_back(std::move(path));
    }
    if (mode != FixtureMode::Folder) {
        UINT index = 0;
        result.error = dialog->GetFileTypeIndex(&index);
        if (FAILED(result.error)) return result;
        if (!index || index > 2) { result.error = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); return result; }
        result.filter_index = index - 1;
    }
    return result;
}

struct BrowseContext { const std::wstring& initial; bool delayed; };
void CALLBACK RevealBrowseEdit(HWND hwnd, UINT, UINT_PTR timer, DWORD) {
    KillTimer(hwnd, timer);
    EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
        wchar_t name[32]{};
        if (GetClassNameW(child, name, static_cast<int>(std::size(name))) && std::wstring_view(name) == L"Edit")
            ShowWindow(child, SW_SHOW);
        return TRUE;
    }, 0);
}
int CALLBACK BrowseCallback(HWND hwnd, UINT message, LPARAM, LPARAM data) {
    if (message == BFFM_INITIALIZED) {
        const auto& context = *reinterpret_cast<BrowseContext*>(data);
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, reinterpret_cast<LPARAM>(context.initial.c_str()));
        if (context.delayed) {
            EnumChildWindows(hwnd, [](HWND child, LPARAM) -> BOOL {
                wchar_t name[32]{};
                if (GetClassNameW(child, name, static_cast<int>(std::size(name))) && std::wstring_view(name) == L"Edit")
                    ShowWindow(child, SW_HIDE);
                return TRUE;
            }, 0);
            SetTimer(hwnd, 41, 2400, RevealBrowseEdit);
        }
    }
    return 0;
}

DialogResult ShowLegacy(HWND owner, const std::wstring& initial_path, bool delayed) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    wchar_t display_name[MAX_PATH]{};
    browse.pszDisplayName = display_name;
    browse.lpszTitle = L"Pulse native dialog fixture: legacy-folder";
    browse.ulFlags = BIF_EDITBOX | BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
    browse.lpfn = BrowseCallback;
    BrowseContext context{initial_path, delayed};
    browse.lParam = reinterpret_cast<LPARAM>(&context);
    PIDLIST_ABSOLUTE raw = SHBrowseForFolderW(&browse);
    std::unique_ptr<ITEMIDLIST, TaskFree> owned(reinterpret_cast<ITEMIDLIST*>(raw));
    DialogResult result;
    if (!raw) { result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED); return result; }
    ComPtr<IShellItem> item;
    result.error = SHCreateItemFromIDList(raw, IID_PPV_ARGS(&item));
    if (FAILED(result.error)) return result;
    std::wstring path;
    result.error = ItemPath(item.Get(), path);
    if (SUCCEEDED(result.error)) result.paths.push_back(std::move(path));
    return result;
}

DialogResult ShowClassic(HWND owner, const std::wstring& initial_path) {
    DialogResult result;
    HMODULE library = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return result;
    const auto show = std::bit_cast<decltype(&GetOpenFileNameW)>(GetProcAddress(library, "GetOpenFileNameW"));
    wchar_t filename[32768]{};
    OPENFILENAMEW spec{sizeof(spec)};
    spec.hwndOwner = owner;
    spec.lpstrFilter = L"All files (*.*)\0*.*\0Text documents (*.txt)\0*.txt\0\0";
    spec.lpstrFile = filename; spec.nMaxFile = static_cast<DWORD>(std::size(filename));
    spec.lpstrInitialDir = initial_path.c_str();
    spec.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (show && show(&spec)) {
        result.error = S_OK;
        result.paths.emplace_back(filename);
        result.filter_index = spec.nFilterIndex - 1;
    } else result.error = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    FreeLibrary(library);
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 3;
    FixtureMode mode = FixtureMode::Open;
    std::wstring_view mode_text = argc > 1 ? argv[1] : L"";
    const bool unowned = mode_text.starts_with(L"unowned-");
    if (unowned) mode_text.remove_prefix(8);
    const bool delayed = mode_text == L"delayed-legacy-folder";
    const bool classic = mode_text == L"classic-open";
    if (delayed) mode_text = L"legacy-folder";
    if (classic) mode_text = L"open";
    if (argc < 3 || argc > 4 || !ParseMode(mode_text, mode) || !argv[2][0] ||
        (argc == 4 && (mode == FixtureMode::Folder || mode == FixtureMode::LegacyFolder))) {
        std::fputs("Usage: pulse_native_dialog_fixture.exe open|save|folder|legacy-folder|opaque-open <initial-directory> [filename]\n", stderr);
        DialogResult error;
        error.error = E_INVALIDARG;
        return EmitResult("unknown", std::move(error), 2);
    }
    DialogResult result;
    result.error = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(result.error)) return EmitResult(ModeName(mode), std::move(result));
    ComScope com_scope;
    pulse::compat::EnableDpiAwareness();
    ComPtr<IShellItem> initial;
    result.error = SHCreateItemFromParsingName(argv[2], nullptr, IID_PPV_ARGS(&initial));
    if (FAILED(result.error)) return EmitResult(ModeName(mode), std::move(result));
    SFGAOF attributes = 0;
    result.error = initial->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes);
    if (FAILED(result.error)) return EmitResult(ModeName(mode), std::move(result));
    if ((attributes & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) {
        result.error = HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        return EmitResult(ModeName(mode), std::move(result));
    }
    WindowScope owner;
    owner.window = CreateWindowExW(0, L"STATIC", L"Pulse native dialog fixture owner", WS_POPUP,
        0, 0, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner.window) {
        const DWORD error = GetLastError();
        result.error = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
        return EmitResult(ModeName(mode), std::move(result));
    }
    const std::wstring filename = argc == 4 ? argv[3] : mode == FixtureMode::Save ? L"fixture-new.txt" : L"";
    const HWND dialog_owner = unowned ? nullptr : owner.window;
    result = classic ? ShowClassic(dialog_owner, argv[2])
        : mode == FixtureMode::LegacyFolder ? ShowLegacy(dialog_owner, argv[2], delayed)
        : ShowModern(dialog_owner, mode, initial.Get(), filename);
    return EmitResult(ModeName(mode), std::move(result));
}
