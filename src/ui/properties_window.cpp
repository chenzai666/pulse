#include "../common/windows_compat.h"
#include "properties_window.h"
#include "window_helpers.h"
#include "../common/localization.h"
#include "../common/text_format.h"

#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>

namespace pulse::ui {
namespace {

constexpr wchar_t kClass[] = L"PulsePropertiesWindow";
constexpr UINT_PTR kReadTimer = 1;
constexpr float kBodyTop = 136.0f;
constexpr float kFooter = 72.0f;
constexpr float kRow = 28.0f;
std::atomic_uint32_t active_reads{0};

struct ActiveRead {
    ~ActiveRead() { active_reads.fetch_sub(1, std::memory_order_relaxed); }
};

bool AcquireReadSlot() {
    auto active = active_reads.load(std::memory_order_relaxed);
    while (active < 2) {
        if (active_reads.compare_exchange_weak(active, active + 1, std::memory_order_relaxed)) return true;
    }
    return false;
}

std::wstring DisplayType(const std::wstring& type) {
    if (type == L"folder") return l10n::Pick(L"文件夹", L"Folder");
    if (type == L"mixed") return l10n::Pick(L"多种类型", L"Multiple types");
    if (type == L"file") return l10n::Pick(L"文件", L"File");
    return type.empty() ? l10n::Pick(L"未知", L"Unknown") : type;
}

std::wstring AttributeState(int state) {
    if (state == 2) return l10n::Pick(L"混合", L"Mixed");
    return state == 1 ? l10n::Pick(L"是", L"Yes") : l10n::Pick(L"否", L"No");
}

std::wstring SizeText(uint64_t size, bool complete) {
    std::wstring value = format::ByteSize(size) + L" (" + format::GroupedInt(size) +
        l10n::Pick(L" 字节)", L" bytes)");
    if (!complete) value = l10n::Pick(L"已统计 ", L"Counted ") + value;
    return value;
}

std::wstring LeafName(const std::wstring& path) {
    const auto end = path.find_last_not_of(L"\\/");
    if (end == std::wstring::npos) return path;
    const auto slash = path.find_last_of(L"\\/", end);
    return path.substr(slash == std::wstring::npos ? 0 : slash + 1,
        end - (slash == std::wstring::npos ? 0 : slash + 1) + 1);
}

} // namespace

struct PropertiesWindow::Request {
    bool started = false; // UI thread only; waiting requests never own a worker.
    std::atomic_bool cancelled{false};
    std::atomic<DWORD> failure{ERROR_SUCCESS};
    std::mutex mutex;
    std::shared_ptr<fs::ItemProperties> result;
};

PropertiesWindow::PropertiesWindow() : painter_(&compositor_) {}
PropertiesWindow::~PropertiesWindow() { Destroy(); }

bool PropertiesWindow::Create(HWND owner, std::function<void(const std::vector<std::wstring>&)> system_properties) {
    if (hwnd_ && owner_ == owner) {
        system_properties_ = std::move(system_properties);
        return true;
    }
    Destroy();
    if (!owner || !IsWindow(owner)) return false;
    owner_ = owner;
    system_properties_ = std::move(system_properties);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!GetClassInfoExW(wc.hInstance, kClass, &wc) && !RegisterClassExW(&wc)) {
        owner_ = nullptr;
        system_properties_ = {};
        return false;
    }
    const UINT dpi = compat::WindowDpi(owner);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass,
        l10n::Pick(L"属性", L"Properties"), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(520, dpi, 96), MulDiv(620, dpi, 96),
        owner, nullptr, wc.hInstance, this);
    if (!hwnd_) { owner_ = nullptr; system_properties_ = {}; }
    return hwnd_ != nullptr;
}

void PropertiesWindow::CancelRead() {
    if (hwnd_) KillTimer(hwnd_, kReadTimer);
    if (request_) request_->cancelled.store(true, std::memory_order_relaxed);
    request_.reset();
    loading_ = false;
}

void PropertiesWindow::Destroy() {
    CancelRead();
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    owner_ = nullptr;
    system_properties_ = {};
    paths_.clear();
    properties_ = {};
    hover_ = pressed_ = 0;
}

void PropertiesWindow::Close() {
    CancelRead();
    pressed_ = hover_ = 0;
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void PropertiesWindow::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (hwnd_) {
        backdrop_ = ApplyBackdrop(hwnd_, dark_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void PropertiesWindow::Show(const std::vector<std::wstring>& paths) {
    if (!hwnd_ || paths.empty()) return;
    if (loading_ && paths == paths_) return;
    CancelRead();
    if (GetCapture() == hwnd_) ReleaseCapture();
    paths_ = paths;
    properties_ = {};
    properties_.name = paths.size() == 1 ? LeafName(paths.front())
        : std::to_wstring(paths.size()) + l10n::Pick(L" 个项目", L" items");
    loading_ = true;
    scroll_ = 0;
    hover_ = pressed_ = 0;
    focus_ = 3;
    try {
        request_ = std::make_shared<Request>();
        TryStartRead();
    } catch (...) {
        CancelRead();
        properties_.size_complete = false;
        properties_.unreadable_count = 1;
        properties_.first_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (loading_ && !SetTimer(hwnd_, kReadTimer, 80, nullptr)) {
        CancelRead();
        properties_.size_complete = false;
        properties_.unreadable_count = 1;
        properties_.first_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (!IsWindowVisible(hwnd_)) {
        const UINT dpi = compat::WindowDpi(owner_);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = (std::min)(MulDiv(520, dpi, 96), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        const int height = (std::min)(MulDiv(620, dpi, 96), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top));
        CenterOwnedWindow(hwnd_, owner_, width, height);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

void PropertiesWindow::TryStartRead() {
    if (!request_ || request_->started || !AcquireReadSlot()) return;
    const auto request = request_;
    request->started = true;
    try {
        // A blocked filesystem call retains only the request and paths. The
        // active slot is released on actual worker exit, never on UI cancellation.
        std::thread([request, paths = paths_] {
            const ActiveRead active;
            try {
                auto result = std::make_shared<fs::ItemProperties>(fs::ReadItemProperties(paths, request->cancelled));
                if (request->cancelled.load(std::memory_order_relaxed)) return;
                std::lock_guard lock(request->mutex);
                request->result = std::move(result);
            } catch (...) {
                request->failure.store(ERROR_NOT_ENOUGH_MEMORY, std::memory_order_release);
            }
        }).detach();
    } catch (...) {
        active_reads.fetch_sub(1, std::memory_order_relaxed);
        request->failure.store(ERROR_NOT_ENOUGH_MEMORY, std::memory_order_release);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PropertiesWindow::Poll() {
    if (!request_) return;
    TryStartRead();
    if (!request_->started) return;
    std::shared_ptr<fs::ItemProperties> result;
    {
        std::lock_guard lock(request_->mutex);
        result = std::move(request_->result);
    }
    if (!result) {
        const DWORD failure = request_->failure.load(std::memory_order_acquire);
        if (failure == ERROR_SUCCESS) return;
        properties_.size_complete = false;
        properties_.unreadable_count = 1;
        properties_.first_error = failure;
    } else {
        if (result->name.empty()) result->name = properties_.name;
        properties_ = std::move(*result);
    }
    CancelRead();
    Scroll(0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

float PropertiesWindow::ContentHeight() const {
    return 412.0f + (loading_ || !properties_.size_complete || properties_.unreadable_count || properties_.skipped_links ? 84.0f : 0.0f);
}

float PropertiesWindow::MaxScroll() const {
    const float height = static_cast<float>(compositor_.Height()) / scale_;
    return (std::max)(0.0f, ContentHeight() - (height - kBodyTop - kFooter));
}

void PropertiesWindow::Scroll(float amount) {
    const float next = std::clamp(scroll_ + amount, 0.0f, MaxScroll());
    if (next != scroll_) { scroll_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
}

D2D1_RECT_F PropertiesWindow::ButtonRect(int control) const {
    const float width = static_cast<float>(compositor_.Width()) / scale_;
    const float height = static_cast<float>(compositor_.Height()) / scale_;
    if (control == 1) return DipRect(scale_, width - 44, 8, 32, 32);
    if (control == 2) return DipRect(scale_, 20, height - 52, 184, 32);
    if (control == 3) return DipRect(scale_, width - 108, height - 52, 88, 32);
    return {};
}

int PropertiesWindow::HitTest(float x, float y) const {
    for (int control = 1; control <= 3; ++control)
        if ((control != 2 || system_properties_) && ContainsRect(ButtonRect(control), x, y)) return control;
    return 0;
}

void PropertiesWindow::Activate(int control) {
    if (control == 1 || control == 3) { Close(); return; }
    if (control == 2 && system_properties_) {
        const auto callback = system_properties_;
        const auto paths = paths_;
        Close();
        callback(paths);
    }
}

void PropertiesWindow::Render(bool present) {
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
    }
    if (!compositor_.Dc()) return;
    const bool contrast = contrast_override_ < 0 ? IsHighContrast() : contrast_override_ != 0;
    const Theme theme = contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    BeginSurface(compositor_, painter_, theme, dark_, contrast, backdrop_, scale_);
    const float width = static_cast<float>(compositor_.Width()) / scale_;
    const float height = static_cast<float>(compositor_.Height()) / scale_;
    const auto text = [&](const std::wstring& value, float x, float y, float w, float h,
                          bool heading = false, bool secondary = false, bool path = false) {
        auto* font = heading ? compositor_.HeaderFormat() : compositor_.SmallFormat();
        const auto measure = [&](std::wstring_view str) {
            float measured = 0;
            if (compositor_.MeasureLumaText(str, font, measured)) return measured;
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(compositor_.DwriteFactory()->CreateTextLayout(str.data(), static_cast<UINT32>(str.size()),
                    font, 1000000.0f, 1000000.0f, &layout))) {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics))) return metrics.widthIncludingTrailingWhitespace;
            }
            return static_cast<float>(str.size()) * 14.0f * scale_;
        };
        const auto fitted = path ? FitPathMiddle(value, w * scale_, measure) : FitTextEnd(value, w * scale_, measure);
        painter_.DrawText(fitted, DipRect(scale_, x, y, w, h), font, secondary ? theme.text_secondary : theme.text);
    };
    const auto button = [&](int control, const std::wstring& label) {
        fluent::ButtonSpec spec;
        spec.bounds = ButtonRect(control);
        spec.text = label;
        spec.icon_only = control == 1;
        spec.glyph = control == 1 ? L"\xE711" : L"";
        spec.kind = control == 1 ? fluent::ButtonKind::Transparent : fluent::ButtonKind::Standard;
        spec.state.enabled = control != 2 || static_cast<bool>(system_properties_);
        spec.state.hovered = hover_ == control;
        spec.state.pressed = pressed_ == control;
        painter_.DrawButton(spec);
        if (focus_ == control && GetFocus() == hwnd_) painter_.DrawFocusRing(spec.bounds, 4 * scale_);
    };
    text(l10n::Pick(L"属性", L"Properties"), 20, 10, width - 80, 26, true);
    button(1, L"");
    const auto icon = DipRect(scale_, 20, 62, 52, 52);
    painter_.FillRoundedRect(icon, 10 * scale_, theme.surface_card);
    painter_.DrawGlyph(properties_.type == L"folder" ? L"\xE8B7" : paths_.size() > 1 ? L"\xE8F1" : L"\xE8A5",
        DipRect(scale_, 30, 72, 32, 32), contrast ? theme.text : theme.accent);
    text(properties_.name, 86, 64, width - 110, 26, true);
    text(loading_ ? l10n::Pick(L"正在读取详细信息…", L"Reading details…") : DisplayType(properties_.type),
        86, 94, width - 110, 20, false, true);

    auto* dc = compositor_.Dc();
    const float view_height = (std::max)(0.0f, height - kBodyTop - kFooter);
    dc->PushAxisAlignedClip(DipRect(scale_, 0, kBodyTop, width, view_height), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = kBodyTop - scroll_;
    if (loading_ || !properties_.size_complete || properties_.unreadable_count || properties_.skipped_links) {
        const auto bounds = DipRect(scale_, 20, y, width - 40, 72);
        painter_.FillRoundedRect(bounds, 8 * scale_, theme.surface_card);
        painter_.StrokeRoundedRect(bounds, 8 * scale_, theme.stroke_divider);
        std::wstring status = l10n::Pick(L"正在统计大小和项目数…", L"Calculating size and item count…");
        std::wstring detail = l10n::Pick(L"大文件夹或网络位置可能需要较长时间", L"Large folders or network locations may take longer");
        if (loading_ && request_ && !request_->started) {
            status = l10n::Pick(L"等待先前的文件读取返回", L"Waiting for earlier file reads to return");
            detail = l10n::Pick(L"读取结束后自动继续；按 Esc 可关闭", L"Continues automatically when ready; Esc closes");
        }
        if (!loading_) {
            status = properties_.size_complete ? l10n::Pick(L"统计完成，已跳过链接目标", L"Complete; link targets skipped")
                : l10n::Pick(L"统计未完成，下方仅显示已读取数据", L"Incomplete; values include only readable items");
            detail = std::to_wstring(properties_.unreadable_count) + l10n::Pick(L" 项无法读取", L" unreadable items") +
                L" · " + std::to_wstring(properties_.skipped_links) + l10n::Pick(L" 个链接未跟随", L" links not followed");
            if (properties_.first_error != ERROR_SUCCESS) detail +=
                l10n::Pick(L" · 错误 ", L" · Error ") + std::to_wstring(properties_.first_error);
        }
        text(status, 32, y + 10, width - 64, 22);
        text(detail, 32, y + 36, width - 64, 20, false, true);
        if (loading_) {
            fluent::ProgressSpec progress;
            progress.bounds = DipRect(scale_, 32, y + 62, width - 64, 3);
            progress.indeterminate = true;
            painter_.DrawProgressBar(progress);
        }
        y += 84;
    }
    const std::wstring unknown = loading_ ? l10n::Pick(L"正在读取…", L"Reading…") : l10n::Pick(L"不可用", L"Unavailable");
    const auto card = [&](const std::wstring& title, int rows) {
        const auto bounds = DipRect(scale_, 20, y, width - 40, 36 + static_cast<float>(rows) * kRow);
        painter_.FillRoundedRect(bounds, 8 * scale_, theme.surface_card);
        painter_.StrokeRoundedRect(bounds, 8 * scale_, theme.stroke_divider);
        text(title, 32, y + 8, width - 64, 22);
        y += 36;
    };
    const auto row = [&](const std::wstring& label, const std::wstring& value, bool path = false) {
        text(label, 32, y, 94, 22, false, true);
        text(value.empty() ? unknown : value, 132, y, width - 164, 22, false, false, path);
        y += kRow;
    };
    card(l10n::Pick(L"常规", L"General"), 5);
    row(l10n::Pick(L"类型", L"Type"), loading_ ? unknown : DisplayType(properties_.type));
    row(l10n::Pick(L"位置", L"Location"), properties_.location.empty() && paths_.size() > 1 && !loading_ && properties_.first_error == ERROR_SUCCESS
        ? l10n::Pick(L"多个位置", L"Multiple locations") : properties_.location, true);
    row(l10n::Pick(L"大小", L"Size"), loading_ ? unknown : SizeText(properties_.size_bytes, properties_.size_complete));
    row(l10n::Pick(L"占用空间", L"Size on disk"), loading_ ? unknown : SizeText(properties_.allocated_bytes, properties_.size_complete));
    row(l10n::Pick(L"包含", L"Contains"), loading_ ? unknown : format::GroupedInt(properties_.file_count) +
        l10n::Pick(L" 个文件，", L" files, ") + format::GroupedInt(properties_.folder_count) + l10n::Pick(L" 个文件夹", L" folders"));
    y += 12;
    card(l10n::Pick(L"时间", L"Dates"), 3);
    row(l10n::Pick(L"创建时间", L"Created"), properties_.has_times ? format::LocalFileTime(properties_.created, unknown) : unknown);
    row(l10n::Pick(L"修改时间", L"Modified"), properties_.has_times ? format::LocalFileTime(properties_.modified, unknown) : unknown);
    row(l10n::Pick(L"访问时间", L"Accessed"), properties_.has_times ? format::LocalFileTime(properties_.accessed, unknown) : unknown);
    y += 12;
    card(l10n::Pick(L"属性（只读展示）", L"Attributes (view only)"), 2);
    row(l10n::Pick(L"只读", L"Read-only"), loading_ || properties_.first_error != ERROR_SUCCESS ? unknown : AttributeState(properties_.readonly_state));
    row(l10n::Pick(L"隐藏", L"Hidden"), loading_ || properties_.first_error != ERROR_SUCCESS ? unknown : AttributeState(properties_.hidden_state));
    dc->PopAxisAlignedClip();
    if (MaxScroll() > 0) {
        fluent::ScrollbarSpec scrollbar;
        scrollbar.viewport = DipRect(scale_, 0, kBodyTop, width, view_height);
        scrollbar.content_extent = ContentHeight() * scale_;
        scrollbar.viewport_extent = view_height * scale_;
        scrollbar.offset = scroll_ * scale_;
        painter_.DrawScrollbar(scrollbar);
    }
    button(2, l10n::Pick(L"系统高级属性", L"System properties…"));
    button(3, l10n::Pick(L"关闭", L"Close"));
    if (present) EndSurface(compositor_);
    else {
        const HRESULT result = dc->EndDraw();
        if (FAILED(result)) compositor_.NotifyDeviceLost(result);
    }
}

LRESULT CALLBACK PropertiesWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PropertiesWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<PropertiesWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_NCDESTROY && self) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->owner_ = nullptr;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self ? self->HandleMessage(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT PropertiesWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(compat::WindowDpi(hwnd_)) / 96.0f;
        backdrop_ = ApplyBackdrop(hwnd_, dark_);
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        return 0;
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: return BorderlessHitTest(hwnd_, lparam, 44 * scale_, ButtonRect(1));
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {static_cast<LONG>(320 * scale_), static_cast<LONG>(360 * scale_)};
        return 0;
    }
    case WM_SIZE:
        if (LOWORD(lparam) && HIWORD(lparam)) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
        Scroll(0);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        const auto* bounds = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
            bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Scroll(0);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        backdrop_ = ApplyBackdrop(hwnd_, dark_);
        painter_.InvalidateTypography();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_TIMER: if (wparam == kReadTimer) { Poll(); return 0; } break;
    case WM_MOUSEMOVE: {
        const int hit = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (hover_ != hit) { hover_ = hit; InvalidateRect(hwnd_, nullptr, FALSE); }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE: hover_ = 0; InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd_);
        pressed_ = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (pressed_) { focus_ = pressed_; SetCapture(hwnd_); }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int pressed = pressed_;
        pressed_ = 0;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (pressed && pressed == HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)))) Activate(pressed);
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED: pressed_ = 0; InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_MOUSEWHEEL: Scroll(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 72); return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) Close();
        else if (wparam == VK_F5) { const auto paths = paths_; Show(paths); }
        else if (wparam == VK_TAB) {
            do { focus_ = (focus_ - 1 + ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 1)) % 3 + 1; }
            while (focus_ == 2 && !system_properties_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else if (wparam == VK_RETURN || wparam == VK_SPACE) { if (!(lparam & (1LL << 30))) Activate(focus_); }
        else if (wparam == VK_DOWN || wparam == VK_NEXT) Scroll(wparam == VK_DOWN ? kRow : 160);
        else if (wparam == VK_UP || wparam == VK_PRIOR) Scroll(wparam == VK_UP ? -kRow : -160);
        else if (wparam == VK_HOME) Scroll(-MaxScroll());
        else if (wparam == VK_END) Scroll(MaxScroll());
        else break;
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTTAB | DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_CLOSE: Close(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: CancelRead(); compositor_.Shutdown(); return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace pulse::ui
