// 独立属性窗口画廊：实际 Render(false) 截图及异步生命周期验证。
// pulse_properties_gallery.exe [--lang zh-CN|en-US] [输出目录]
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/properties_window.h"
#include "../ui/window_helpers.h"

#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct PropertiesWindowTestAccess {
    static bool RegionHasContent(IWICFormatConverter* converter, WICRect region) {
        const UINT stride = static_cast<UINT>(region.Width) * 4;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * region.Height);
        if (FAILED(converter->CopyPixels(&region, stride, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
        for (size_t i = 4; i < pixels.size(); i += 4)
            if (pixels[i] != pixels[0] || pixels[i + 1] != pixels[1] || pixels[i + 2] != pixels[2]) return true;
        return false;
    }

    static bool VerifyPixels(const std::wstring& file, float scale, int width, int height) {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return false;
        UINT actual_width = 0, actual_height = 0;
        if (FAILED(frame->GetSize(&actual_width, &actual_height)) || actual_width != static_cast<UINT>(width * scale) ||
            actual_height != static_cast<UINT>(height * scale)) return false;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) || FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        return RegionHasContent(converter.get(), {static_cast<INT>(20 * scale), static_cast<INT>(10 * scale),
                   static_cast<INT>(100 * scale), static_cast<INT>(26 * scale)}) &&
            RegionHasContent(converter.get(), {static_cast<INT>(86 * scale), static_cast<INT>(64 * scale),
                   static_cast<INT>((width - 110) * scale), static_cast<INT>(26 * scale)}) &&
            RegionHasContent(converter.get(), {static_cast<INT>(32 * scale), static_cast<INT>(172 * scale),
                   static_cast<INT>((width - 64) * scale), static_cast<INT>(22 * scale)}) &&
            RegionHasContent(converter.get(), {static_cast<INT>(32 * scale), static_cast<INT>((height - 46) * scale),
                   static_cast<INT>(156 * scale), static_cast<INT>(18 * scale)});
    }

    static bool Snapshot(PropertiesWindow& window, const fs::ItemProperties& properties, bool loading,
                         bool multiple, float scale, bool dark, bool contrast, int width, int height,
                         const std::wstring& output, bool scroll_to_end = false) {
        window.CancelRead();
        window.scale_ = scale;
        window.dark_ = dark;
        window.contrast_override_ = contrast ? 1 : 0;
        window.backdrop_ = false;
        window.properties_ = properties;
        window.loading_ = loading;
        window.paths_ = multiple ? std::vector<std::wstring>{L"C:\\Example\\a", L"D:\\Example\\b"}
                                 : std::vector<std::wstring>{L"C:\\Example\\Report.docx"};
        window.scroll_ = 0;
        window.compositor_.Resize(static_cast<int>(width * scale), static_cast<int>(height * scale));
        window.compositor_.RecreateTextFormats(scale);
        window.painter_.SetCompositor(&window.compositor_);
        window.painter_.SetScale(scale);
        if (scroll_to_end) window.Scroll(window.MaxScroll());
        const auto advanced = window.ButtonRect(2), close = window.ButtonRect(3);
        if (advanced.right > close.left || advanced.bottom > height * scale - 8 * scale) return false;
        if (!window.compositor_.Dc()) return false;
        // Save the freshly drawn back buffer before Present rotates it.
        window.Render(false);
        return !window.compositor_.NeedsRecovery() && window.compositor_.SaveSnapshot(output.c_str()) &&
            VerifyPixels(output, scale, width, height);
    }

    static HWND Handle(PropertiesWindow& window) { return window.hwnd_; }
    static bool Loading(const PropertiesWindow& window) { return window.loading_; }
    static uint64_t Size(const PropertiesWindow& window) { return window.properties_.size_bytes; }
    static const std::wstring& Name(const PropertiesWindow& window) { return window.properties_.name; }
    static bool Cancelled(const PropertiesWindow& window) { return !window.request_ && !window.loading_; }
    static bool InputReset(const PropertiesWindow& window) { return !window.hover_ && !window.pressed_; }
    static int Focus(const PropertiesWindow& window) { return window.focus_; }
};

} // namespace pulse::ui

namespace {

using namespace pulse;
using namespace pulse::ui;

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND CreateOwner() {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = OwnerProc;
    wc.lpszClassName = L"PulsePropertiesGalleryOwner";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"Pulse Properties Gallery", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 700, nullptr, nullptr, wc.hInstance, nullptr);
}

bool PumpUntil(const std::function<bool()>& complete, DWORD timeout = 5000) {
    const ULONGLONG end = GetTickCount64() + timeout;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (complete()) return true;
        Sleep(5);
    } while (GetTickCount64() < end);
    return false;
}

bool WriteFixture(const std::wstring& path, DWORD bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char contents[] = "properties lifecycle fixture text";
    DWORD written = 0;
    const bool ok = WriteFile(file, contents, bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}

int LifecycleTests(const std::wstring& directory, FILE* log) {
    using Access = PropertiesWindowTestAccess;
    const std::wstring fixtures = directory + L"\\properties-lifecycle-" + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(fixtures.c_str(), nullptr)) return 1;
    const std::wstring first = fixtures + L"\\first.txt", second = fixtures + L"\\second.txt";
    if (!WriteFixture(first, 17) || !WriteFixture(second, 31)) {
        DeleteFileW(first.c_str()); DeleteFileW(second.c_str()); RemoveDirectoryW(fixtures.c_str());
        return 1;
    }
    HWND owner = CreateOwner();
    PropertiesWindow window;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        if (!ok) ++failures;
        if (log) std::fprintf(log, "[%s] %s\r\n", ok ? "PASS" : "FAIL", name);
    };
    int advanced_count = 0;
    std::vector<std::wstring> advanced_paths;
    check(owner && window.Create(owner, [&](const auto& paths) { ++advanced_count; advanced_paths = paths; }), "create modeless window");
    if (Access::Handle(window)) {
        HWND hwnd = Access::Handle(window);
        window.SetTheme(false, HexColor(0x0078D4));
        window.Show({first});
        check(IsWindowEnabled(owner), "owner remains enabled");
        check(PumpUntil([&] { return !Access::Loading(window); }) && Access::Size(window) == 17 &&
            Access::Name(window) == L"first.txt", "real Show receives worker result");
        window.Show({first});
        SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        check(Access::Cancelled(window) && !IsWindowVisible(hwnd), "ESC hides and cancels request");
        window.Show({first});
        window.Show({second});
        check(PumpUntil([&] { return !Access::Loading(window); }) && Access::Size(window) == 31 &&
            Access::Name(window) == L"second.txt", "rapid Show retains newest generation");
        const ULONGLONG quiet_until = GetTickCount64() + 250;
        PumpUntil([&] { return GetTickCount64() >= quiet_until; }, 1000);
        check(Access::Size(window) == 31 && Access::Name(window) == L"second.txt", "late old result cannot overwrite newest data");
        SendMessageW(hwnd, WM_KEYDOWN, VK_F5, 0);
        check(PumpUntil([&] { return !Access::Loading(window); }) && Access::Size(window) == 31, "F5 asynchronously refreshes current selection");
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
        check(Access::Focus(window) == 1, "Tab navigates to caption close");
        SendMessageW(hwnd, WM_KEYDOWN, VK_TAB, 0);
        check(Access::Focus(window) == 2, "Tab navigates to advanced properties");
        SendMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0);
        check(advanced_count == 1 && advanced_paths == std::vector<std::wstring>{second} &&
            !IsWindowVisible(hwnd) && Access::Cancelled(window), "Enter invokes retained advanced callback");
        window.Show({second});
        RECT client{}; GetClientRect(hwnd, &client);
        const LPARAM caption = MAKELPARAM(client.right - 24, 24);
        SendMessageW(hwnd, WM_MOUSEMOVE, 0, caption);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, caption);
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
        check(Access::Cancelled(window) && Access::InputReset(window) && GetCapture() != hwnd,
            "caption close cancellation clears hover press capture");
        window.Show({second});
        SendMessageW(hwnd, WM_KEYDOWN, VK_SPACE, 0);
        check(!IsWindowVisible(hwnd) && Access::Cancelled(window), "Space activates focused footer Close");
        window.Show({first});
        DestroyWindow(owner);
        owner = nullptr;
        check(!Access::Handle(window) && !window.owner() && Access::Cancelled(window), "owner destruction cancels pending worker");
        const ULONGLONG late_until = GetTickCount64() + 250;
        PumpUntil([&] { return GetTickCount64() >= late_until; }, 1000);
        check(!Access::Handle(window), "worker completion after owner destruction cannot resurrect HWND");
    }
    window.Destroy();
    if (owner) DestroyWindow(owner);
    DeleteFileW(first.c_str()); DeleteFileW(second.c_str()); RemoveDirectoryW(fixtures.c_str());
    return failures;
}

int RunGallery(const std::wstring& directory) {
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 1;
    FILE* log = nullptr;
    _wfopen_s(&log, (directory + L"\\properties-gallery.txt").c_str(), L"wb");
    int failures = LifecycleTests(directory, log);
    HWND owner = CreateOwner();
    PropertiesWindow window;
    if (!owner || !window.Create(owner, [](const auto&) {})) {
        if (owner) DestroyWindow(owner);
        if (log) { std::fprintf(log, "[FAIL] gallery compositor initialization\r\n"); std::fclose(log); }
        return 2;
    }
    fs::ItemProperties file;
    file.name = l10n::Pick(L"季度财务报告与预算附件 2026.docx", L"Quarterly financial report and budget 2026.docx");
    file.type = L"file";
    file.location = L"C:\\Users\\Example\\Documents\\Projects\\2026\\Quarterly reports\\Approved documents";
    file.size_bytes = 2876134; file.allocated_bytes = 2879488; file.file_count = 1;
    file.has_times = true;
    SYSTEMTIME time{2026, 10, 0, 12, 9, 35, 0, 0};
    SystemTimeToFileTime(&time, &file.created);
    file.modified = file.accessed = file.created;
    fs::ItemProperties folder = file;
    folder.name = l10n::Pick(L"项目文档", L"Project documents"); folder.type = L"folder";
    folder.size_bytes = 2482834731; folder.allocated_bytes = 2482892800;
    folder.file_count = 1438; folder.folder_count = 52; folder.readonly_state = 1;
    fs::ItemProperties multiple = folder;
    multiple.name = l10n::Pick(L"2 个项目", L"2 items"); multiple.type = L"mixed";
    multiple.location.clear(); multiple.has_times = false; multiple.readonly_state = multiple.hidden_state = 2;
    fs::ItemProperties error = folder;
    error.size_complete = false; error.unreadable_count = 4; error.skipped_links = 2; error.first_error = ERROR_ACCESS_DENIED;
    fs::ItemProperties loading;
    loading.name = l10n::Pick(L"网络共享文档", L"Shared network documents");
    struct Scene { const wchar_t* name; fs::ItemProperties properties; bool loading; bool multi; int width; int height; bool bottom; };
    const Scene scenes[] = {
        {L"file", file, false, false, 520, 620, false},
        {L"folder", folder, false, false, 520, 620, false},
        {L"multi", multiple, false, true, 520, 620, false},
        {L"loading", loading, true, false, 520, 620, false},
        {L"error", error, false, false, 520, 620, false},
        {L"narrow", file, false, false, 320, 480, false},
        {L"narrow_bottom", multiple, false, true, 320, 360, true},
    };
    int written = 0;
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (int variant = 0; variant < 3; ++variant) {
            for (const auto& scene : scenes) {
                const wchar_t* theme = variant == 0 ? L"light" : variant == 1 ? L"dark" : L"hc";
                const std::wstring filename = L"properties_" + std::wstring(scene.name) + L"_" + theme + L"_" +
                    std::to_wstring(static_cast<int>(scale * 100)) + L".png";
                const bool ok = PropertiesWindowTestAccess::Snapshot(window, scene.properties, scene.loading, scene.multi,
                    scale, variant == 1, variant == 2, scene.width, scene.height, directory + L"\\" + filename, scene.bottom);
                if (ok) ++written; else ++failures;
                if (log) std::fprintf(log, "[%s] %ls\r\n", ok ? "PASS" : "FAIL", filename.c_str());
            }
        }
    }
    window.Destroy();
    DestroyWindow(owner);
    if (log) { std::fprintf(log, "written=%d failed=%d\r\n", written, failures); std::fclose(log); }
    return failures ? 4 : 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    compat::EnableDpiAwareness();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring directory = L"build-properties-verify";
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--lang" && i + 1 < argc) l10n::SetLanguage(argv[++i]);
        else directory = argv[i];
    }
    if (argv) LocalFree(argv);
    const int result = RunGallery(directory);
    CoUninitialize();
    return result;
}
