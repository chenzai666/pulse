#include "../common/windows_compat.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <oleidl.h>
#include <wrl/client.h>

#include <cstdio>
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
constexpr UINT_PTR kOpaqueCancelTimer = 0x50554c53;
constexpr UINT kOpaqueCancelTimeout = 15000;
constexpr wchar_t kOpaqueDialogProperty[] = L"Pulse.NativeFixture.OpaqueDialog";

struct DialogResult {
    HRESULT error = E_FAIL;
    std::vector<std::wstring> paths;
    UINT filter_index = 0;
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

void CALLBACK CancelOpaqueDialog(HWND owner, UINT, UINT_PTR timer, DWORD) {
    KillTimer(owner, timer);
    auto* dialog = reinterpret_cast<IFileDialog*>(GetPropW(owner, kOpaqueDialogProperty));
    if (dialog) dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
}

class OpaqueCancellation final : public IFileDialogEvents {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
        *output = static_cast<IFileDialogEvents*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog* dialog, IShellItem*) override { Arm(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override { Arm(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog* dialog) override { Arm(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (!response) return E_POINTER;
        *response = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog* dialog) override { Arm(dialog); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override {
        if (!response) return E_POINTER;
        *response = FDEOR_DEFAULT;
        return S_OK;
    }
    void Clear() {
        if (!window_) return;
        DWORD process = 0;
        if (GetWindowThreadProcessId(window_, &process) == GetCurrentThreadId() && process == GetCurrentProcessId() &&
            GetPropW(window_, kOpaqueDialogProperty) == dialog_) {
            KillTimer(window_, kOpaqueCancelTimer);
            RemovePropW(window_, kOpaqueDialogProperty);
        }
        window_ = nullptr;
        dialog_ = nullptr;
    }
private:
    void Arm(IFileDialog* dialog) {
        if (window_) return;
        ComPtr<IOleWindow> ole_window;
        HWND window = nullptr;
        DWORD process = 0;
        if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&ole_window))) || FAILED(ole_window->GetWindow(&window)) ||
            !window || GetWindowThreadProcessId(window, &process) != GetCurrentThreadId() || process != GetCurrentProcessId()) return;
        if (!SetPropW(window, kOpaqueDialogProperty, dialog)) return;
        if (!SetTimer(window, kOpaqueCancelTimer, kOpaqueCancelTimeout, CancelOpaqueDialog)) {
            RemovePropW(window, kOpaqueDialogProperty);
            return;
        }
        window_ = window;
        dialog_ = dialog;
    }
    LONG references_ = 1;
    HWND window_ = nullptr;
    IFileDialog* dialog_ = nullptr;
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
    ComPtr<OpaqueCancellation> cancellation;
    DWORD cancellation_cookie = 0;
    if (mode == FixtureMode::OpaqueOpen) {
        cancellation.Attach(new OpaqueCancellation());
        result.error = dialog->Advise(cancellation.Get(), &cancellation_cookie);
        if (FAILED(result.error)) return result;
    }
    result.error = dialog->Show(owner);
    if (cancellation) {
        cancellation->Clear();
        dialog->Unadvise(cancellation_cookie);
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

int CALLBACK BrowseCallback(HWND hwnd, UINT message, LPARAM, LPARAM data) {
    if (message == BFFM_INITIALIZED) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

DialogResult ShowLegacy(HWND owner, const std::wstring& initial_path) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    wchar_t display_name[MAX_PATH]{};
    browse.pszDisplayName = display_name;
    browse.lpszTitle = L"Pulse native dialog fixture: legacy-folder";
    browse.ulFlags = BIF_EDITBOX | BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
    browse.lpfn = BrowseCallback;
    browse.lParam = reinterpret_cast<LPARAM>(initial_path.c_str());
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
}

int wmain(int argc, wchar_t** argv) {
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 3;
    FixtureMode mode = FixtureMode::Open;
    if (argc < 3 || argc > 4 || !ParseMode(argv[1], mode) || !argv[2][0] ||
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
    result = mode == FixtureMode::LegacyFolder ? ShowLegacy(owner.window, argv[2])
        : ShowModern(owner.window, mode, initial.Get(), filename);
    return EmitResult(ModeName(mode), std::move(result));
}
