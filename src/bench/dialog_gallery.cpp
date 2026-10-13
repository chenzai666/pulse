// Visual contract for Pulse's own confirm dialogs. The open dialog hosts the
// main window's renderer and is exercised with pulse.exe --picker-live.
//
//   pulse_dialog_gallery.exe [--lang zh-CN|en-US] [out_dir]
//       Renders every scene offscreen in light, dark and high contrast at 100%
//       and 150% scale into out_dir (default build-dialog-verify).
//   pulse_dialog_gallery.exe --delete-confirmation <out_dir>
//       Renders only deletion checkbox states at 100%, 150% and 200% scale.
//   pulse_dialog_gallery.exe --status-center <out_dir>
//       Renders the actual status flyout's empty/task fixtures at the same scales.
//   pulse_dialog_gallery.exe --live <kind> [--dark] [--out <file>]
//       Opens the real window. kind: confirm-warning, confirm-danger,
//       confirm-three, confirm-items. The choice is written to --out (UTF-8).

#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/confirm_dialog.h"
#include "../ui/confirm_dialog_view.h"
#include "../ui/fluent_components.h"
#include "../ui/status_center_window.h"

#include <shellapi.h>
#include <windows.h>

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct StatusCenterWindowTestAccess {
    static bool HeaderHasContent(const std::wstring& output, float scale) {
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromFilename(output.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad, &decoder))) return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return false;
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
        const WICRect title{static_cast<INT>(18 * scale), static_cast<INT>(10 * scale),
                            static_cast<INT>(340 * scale), static_cast<INT>(26 * scale)};
        const UINT stride = static_cast<UINT>(title.Width) * 4;
        std::vector<BYTE> pixels(static_cast<size_t>(stride) * title.Height);
        if (FAILED(converter->CopyPixels(&title, stride, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
        // A successful PNG write alone also accepts a blank swap-chain buffer.
        // The title rectangle is inside the flat background, away from borders.
        for (size_t index = 4; index < pixels.size(); index += 4)
            if (pixels[index] != pixels[0] || pixels[index + 1] != pixels[1] || pixels[index + 2] != pixels[2]) return true;
        return false;
    }

    static bool Snapshot(StatusCenterWindow& window, const std::vector<app::StatusTaskRecord>& records,
                         float scale, bool dark, const std::wstring& output) {
        window.scale_ = scale;
        window.dark_ = dark;
        window.backdrop_ = false;
        window.compositor_.Resize(static_cast<int>(464 * scale), static_cast<int>(520 * scale));
        window.compositor_.RecreateTextFormats(scale);
        window.painter_.SetCompositor(&window.compositor_);
        window.painter_.SetScale(scale);
        window.Update(records);
        if (!window.compositor_.Dc()) return false;
        // Capture the rendered back buffer before Present rotates it.
        window.Render(false);
        return !window.compositor_.NeedsRecovery() && window.compositor_.SaveSnapshot(output.c_str()) &&
            HeaderHasContent(output, scale);
    }
};

} // namespace pulse::ui

namespace {

using namespace pulse::ui;
using pulse::l10n::StringId;

LRESULT CALLBACK GalleryWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

struct Variant {
    const wchar_t* name;
    bool dark;
    bool high_contrast;
};

constexpr Variant kVariants[] = {
    {L"light", false, false},
    {L"dark", true, false},
    {L"hc", false, true},
};

Theme ThemeFor(const Variant& v) {
    return v.high_contrast ? MakeHighContrastTheme() : MakeTheme(v.dark, HexColor(0x0078D4));
}

bool RenderScene(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                 bool high_contrast, float scale, int width, int height,
                 const std::wstring& output,
                 const std::function<void()>& draw) {
    compositor.Resize(width, height);
    auto* dc = compositor.Dc();
    if (!dc) return false;
    dc->BeginDraw();
    dc->Clear(theme.bg);
    painter.BeginFrame(theme, high_contrast);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    painter.FillRoundedRect(D2D1::RectF(0, 0, w, h), 0, theme.bg);
    painter.StrokeRoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), 12.0f * scale,
                              theme.stroke_card);
    draw();
    if (FAILED(dc->EndDraw())) return false;
    return compositor.SaveSnapshot(output.c_str());
}

// ---- confirm scenes -------------------------------------------------------

struct ConfirmScene {
    const wchar_t* name;
    ConfirmDialogSpec spec;
    ConfirmDialogVisual visual;
};

std::vector<ConfirmScene> ConfirmScenes() {
    std::vector<ConfirmScene> scenes;
    {
        ConfirmScene s{L"confirm_warning", {}, {}};
        s.spec.title = L"继续导出诊断信息？";
        s.spec.message = L"诊断包可能包含文件路径和最近打开的位置。只在需要排查问题时发送给开发者。";
        s.spec.confirm_text = pulse::l10n::Get(StringId::ConfirmContinue);
        s.spec.tone = ConfirmTone::Warning;
        s.spec.default_choice = ConfirmChoice::Cancel;
        s.visual.focus = kConfirmCancel;
        s.visual.show_focus = true;
        scenes.push_back(std::move(s));
    }
    {
        ConfirmScene s{L"confirm_danger", {}, {}};
        s.spec.title = L"永久删除 7 个项目？";
        s.spec.message = L"这些项目不会进入回收站，删除后无法恢复。";
        s.spec.confirm_text = L"永久删除";
        s.spec.danger = true;
        s.spec.items = {L"C:\\Users\\Example\\Desktop\\报告 2026.docx", L"C:\\Users\\Example\\Desktop\\预算.xlsx",
                        L"C:\\Users\\Example\\Desktop\\照片", L"C:\\Users\\Example\\Desktop\\notes.md",
                        L"C:\\Users\\Example\\Desktop\\demo.mp4", L"C:\\Users\\Example\\Desktop\\a.txt",
                        L"C:\\Users\\Example\\Desktop\\b.txt"};
        s.spec.default_choice = ConfirmChoice::Cancel;
        s.visual.focus = kConfirmCancel;
        s.visual.hover = kConfirmPrimary;
        scenes.push_back(std::move(s));
    }
    {
        ConfirmScene s{L"confirm_three", {}, {}};
        s.spec.title = L"同时导出索引服务的诊断信息？";
        s.spec.message = L"索引服务的日志能帮助排查搜索问题，但会让诊断包更大。";
        s.spec.confirm_text = pulse::l10n::Get(StringId::DiagnosticsIncludeServiceYes);
        s.spec.secondary_text = pulse::l10n::Get(StringId::DiagnosticsIncludeServiceNo);
        s.spec.tone = ConfirmTone::Question;
        s.spec.default_choice = ConfirmChoice::Secondary;
        s.visual.focus = kConfirmSecondary;
        s.visual.show_focus = true;
        scenes.push_back(std::move(s));
    }
    {
        ConfirmScene s{L"confirm_items", {}, {}};
        s.spec.title = L"文件正在被其他程序使用";
        s.spec.message = L"「季度报告.docx」已在下列程序中打开。关闭它们后重试。";
        s.spec.confirm_text = pulse::l10n::Get(StringId::RecoveryRetry);
        s.spec.items = {L"Microsoft Word（WINWORD.EXE）", L"Windows 资源管理器（explorer.exe）"};
        s.spec.item_glyph = L"\xE7C4";
        s.spec.note = L"关闭程序前请先保存它里面的改动。";
        s.spec.tone = ConfirmTone::Warning;
        scenes.push_back(std::move(s));
    }
    for (const bool checked : {false, true}) {
        ConfirmScene s{checked ? L"confirm_delete_checked" : L"confirm_delete_unchecked", {}, {}};
        s.spec.title = pulse::l10n::Get(StringId::Delete);
        s.spec.message = pulse::l10n::Get(StringId::RecycleConfirmOne);
        s.spec.confirm_text = pulse::l10n::Get(StringId::Delete);
        s.spec.items = {L"C:\\Users\\Example\\Desktop\\报告 2026.docx"};
        s.spec.default_choice = ConfirmChoice::Cancel;
        s.spec.checkbox.text = pulse::l10n::Pick(L"永久删除，不放入回收站", L"Permanently delete (skip Recycle Bin)");
        s.spec.checkbox.checked = checked;
        s.spec.checkbox.danger_when_checked = true;
        s.spec.checkbox.checked_message = pulse::l10n::Get(StringId::PermanentDeleteOne);
        s.spec.checkbox.checked_confirm_text = pulse::l10n::Get(StringId::PermanentDelete);
        s.spec.checkbox.checked_note = pulse::l10n::Pick(L"文件不会进入回收站，无法通过回收站还原。",
                                                      L"Files skip the Recycle Bin and cannot be restored from it.");
        s.visual.focus = checked ? kConfirmCheckbox : kConfirmCancel;
        s.visual.show_focus = true;
        scenes.push_back(std::move(s));
    }
    return scenes;
}

int RunGallery(const std::wstring& out_dir, bool delete_only = false) {
    CreateDirectoryW(out_dir.c_str(), nullptr);
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = GalleryWndProc;
    window_class.lpszClassName = L"PulseDialogGallery";
    RegisterClassW(&window_class);
    HWND hwnd = CreateWindowExW(0, window_class.lpszClassName, L"Pulse Dialog Gallery",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                                nullptr, nullptr, window_class.hInstance, nullptr);
    if (!hwnd) return 1;
    Compositor compositor;
    if (!compositor.Init(hwnd)) {
        DestroyWindow(hwnd);
        return 2;
    }
    int failures = 0;
    int written = 0;
    const float scales[] = {1.0f, 1.5f, 2.0f};
    for (const float scale : scales) {
        if (!delete_only && scale > 1.5f) continue;
        compositor.RecreateTextFormats(scale);
        fluent::Painter painter(&compositor);
        painter.SetScale(scale);
        const std::wstring suffix = scale > 1.5f ? L"_200" : scale > 1.0f ? L"_150" : L"";
        for (const Variant& variant : kVariants) {
            const Theme theme = ThemeFor(variant);
            for (const ConfirmScene& scene : ConfirmScenes()) {
                if (delete_only && std::wstring(scene.name).find(L"confirm_delete_") != 0) continue;
                const ConfirmDialogSpec spec = ConfirmSpecForCheckbox(scene.spec, scene.spec.checkbox.checked);
                ConfirmDialogFormats formats;
                formats.Create(compositor.DwriteFactory(), scale);
                const ConfirmDialogLayout layout = LayoutConfirmDialog(
                    spec, painter, compositor.DwriteFactory(), formats, scale);
                const std::wstring out = out_dir + L"\\" + scene.name + L"_" + variant.name +
                                         suffix + L".png";
                const bool ok = RenderScene(
                    compositor, painter, theme, variant.high_contrast, scale,
                    static_cast<int>(layout.width + 0.99f), static_cast<int>(layout.height + 0.99f),
                    out, [&] {
                        DrawConfirmDialog(compositor, painter, theme, spec, layout, formats,
                                          scene.visual, variant.dark, variant.high_contrast);
                    });
                ok ? ++written : ++failures;
            }
        }
    }
    compositor.Shutdown();
    DestroyWindow(hwnd);
    const std::wstring log = out_dir + L"\\gallery.txt";
    FILE* f = nullptr;
    if (_wfopen_s(&f, log.c_str(), L"wb") == 0 && f) {
        std::fprintf(f, "written=%d failed=%d\r\n", written, failures);
        fclose(f);
    }
    return failures == 0 ? 0 : 4;
}

int RunStatusCenterGallery(const std::wstring& out_dir) {
    CreateDirectoryW(out_dir.c_str(), nullptr);
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = GalleryWndProc;
    window_class.lpszClassName = L"PulseStatusCenterGallery";
    RegisterClassW(&window_class);
    HWND owner = CreateWindowExW(0, window_class.lpszClassName, L"Pulse Status Center Gallery",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        nullptr, nullptr, window_class.hInstance, nullptr);
    if (!owner) return 1;
    StatusCenterWindow window;
    if (!window.Create(owner, {})) { DestroyWindow(owner); return 2; }
    std::vector<pulse::app::StatusTaskRecord> tasks;
    pulse::app::StatusTaskRecord copying;
    copying.key = {pulse::app::StatusTaskSource::Operation, 1, nullptr};
    copying.operation = pulse::ops::OpType::Copy;
    copying.state = pulse::app::StatusTaskState::Running;
    copying.progress = 0.42;
    copying.cancellable = true;
    copying.source = L"季度报告与附件";
    copying.item_count = 14;
    copying.detail = L"正在复制：报告 2026.docx";
    tasks.push_back(copying);
    pulse::app::StatusTaskRecord converted;
    converted.key = {pulse::app::StatusTaskSource::FileConversion, 2, owner};
    converted.state = pulse::app::StatusTaskState::Completed;
    converted.progress = 1.0;
    converted.target = L"png";
    converted.source = L"旅行照片.jpg";
    converted.destination = L"D:\\导出\\旅行照片.png";
    tasks.push_back(converted);
    pulse::app::StatusTaskRecord failed;
    failed.key = {pulse::app::StatusTaskSource::Operation, 3, nullptr};
    failed.operation = pulse::ops::OpType::RecycleDelete;
    failed.state = pulse::app::StatusTaskState::Failed;
    failed.source = L"共享文档.docx";
    failed.detail = L"文件正在被其他程序使用，请关闭后重试。";
    tasks.push_back(failed);
    int written = 0;
    int failures = 0;
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        const std::wstring suffix = L"_" + std::to_wstring(static_cast<int>(scale * 100));
        for (const Variant& variant : kVariants) {
            if (variant.high_contrast) continue;
            for (bool empty : {true, false}) {
                const std::wstring name = empty ? L"status_center_empty_" : L"status_center_tasks_";
                const std::wstring output = out_dir + L"\\" + name + variant.name + suffix + L".png";
                const bool ok = StatusCenterWindowTestAccess::Snapshot(window,
                    empty ? std::vector<pulse::app::StatusTaskRecord>{} : tasks, scale, variant.dark, output);
                ok ? ++written : ++failures;
            }
        }
    }
    window.Destroy();
    DestroyWindow(owner);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    FILE* log = nullptr;
    if (_wfopen_s(&log, (out_dir + L"\\status-center-gallery.txt").c_str(), L"wb") == 0 && log) {
        std::fprintf(log, "written=%d failed=%d\r\n", written, failures);
        fclose(log);
    }
    std::printf("status-center gallery: written=%d failed=%d\n", written, failures);
    return failures ? 4 : 0;
}

void WriteResult(const std::wstring& file, const std::wstring& text) {
    if (file.empty()) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr,
                                          nullptr);
    std::string utf8(static_cast<size_t>(bytes > 0 ? bytes - 1 : 0), '\0');
    if (bytes > 1)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") == 0 && f) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
}

int RunLive(const std::wstring& kind, bool dark, const std::wstring& out) {
    const D2D1_COLOR_F accent = HexColor(0x0078D4);
    if (kind.rfind(L"confirm-", 0) == 0) {
        const std::wstring name = L"confirm_" + kind.substr(8);
        for (const ConfirmScene& scene : ConfirmScenes()) {
            if (name != scene.name) continue;
            const ConfirmChoice choice = ShowConfirmDialogEx(nullptr, scene.spec, dark, accent);
            WriteResult(out, choice == ConfirmChoice::Confirm ? L"confirm"
                             : choice == ConfirmChoice::Secondary ? L"secondary" : L"cancel");
            return 0;
        }
        return 5;
    }
    return 5;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    pulse::compat::EnableDpiAwareness();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    std::wstring language = L"zh-CN";
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--lang" && i + 1 < argc) language = argv[++i];
        else args.emplace_back(argv[i]);
    }
    if (argv) LocalFree(argv);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), language);

    int result = 0;
    if (!args.empty() && args[0] == L"--delete-confirmation") {
        result = RunGallery(args.size() > 1 ? args[1] : L"build-delete-confirmation-verify", true);
    } else if (!args.empty() && args[0] == L"--status-center") {
        pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
        result = RunStatusCenterGallery(args.size() > 1 ? args[1] : L"build-status-center-verify");
    } else if (!args.empty() && args[0] == L"--live" && args.size() >= 2) {
        bool dark = false;
        std::wstring out;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == L"--dark") dark = true;
            else if (args[i] == L"--out" && i + 1 < args.size()) out = args[++i];
        }
        result = RunLive(args[1], dark, out);
    } else {
        result = RunGallery(args.empty() ? L"build-dialog-verify" : args[0]);
    }
    CoUninitialize();
    return result;
}
