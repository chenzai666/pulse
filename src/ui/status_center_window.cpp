#include "../common/windows_compat.h"
#include "status_center_window.h"
#include "window_helpers.h"
#include "../common/localization.h"
#include "../common/text_format.h"

#include <windowsx.h>
#include <algorithm>
#include <optional>
#include <utility>

namespace pulse::ui {
namespace {

constexpr wchar_t kClass[] = L"PulseStatusCenterWindow";
constexpr float kHeader = 84.0f;
constexpr float kRow = 132.0f;

std::wstring TaskName(const app::StatusTaskRecord& record) {
    if (record.key.source == app::StatusTaskSource::FileConversion)
        return l10n::Pick(L"格式转换", L"File conversion") + (record.target.empty() ? L"" : L" · " + record.target);
    switch (record.operation) {
    case ops::OpType::Copy: return l10n::Pick(L"复制", L"Copy");
    case ops::OpType::Move: return l10n::Pick(L"移动", L"Move");
    case ops::OpType::RecycleDelete: return l10n::Pick(L"移到回收站", L"Recycle");
    case ops::OpType::RealDelete: return l10n::Pick(L"永久删除", L"Delete permanently");
    case ops::OpType::RestoreRecycle: return l10n::Pick(L"还原", L"Restore");
    case ops::OpType::EmptyRecycle: return l10n::Pick(L"清空回收站", L"Empty Recycle Bin");
    case ops::OpType::Rename:
    case ops::OpType::BatchRename: return l10n::Pick(L"重命名", L"Rename");
    case ops::OpType::CreateFolder: return l10n::Pick(L"新建文件夹", L"New folder");
    default: return l10n::Pick(L"新建文件", L"New file");
    }
}

std::wstring StateName(app::StatusTaskState state) {
    using S = app::StatusTaskState;
    switch (state) {
    case S::Queued: return l10n::Pick(L"等待开始", L"Queued");
    case S::Running: return l10n::Pick(L"正在处理", L"In progress");
    case S::Paused: return l10n::Pick(L"已暂停", L"Paused");
    case S::Waiting: return l10n::Pick(L"等待处理", L"Action required");
    case S::Cancelling: return l10n::Pick(L"正在取消", L"Cancelling");
    case S::Completed: return l10n::Pick(L"已完成", L"Completed");
    case S::CompletedWithErrors: return l10n::Pick(L"部分失败", L"Completed with errors");
    case S::Failed: return l10n::Pick(L"失败", L"Failed");
    default: return l10n::Pick(L"已取消", L"Cancelled");
    }
}

} // namespace

StatusCenterWindow::StatusCenterWindow() : painter_(&compositor_) {}
StatusCenterWindow::~StatusCenterWindow() { Destroy(); }

bool StatusCenterWindow::Create(HWND owner, StatusCenterCallbacks callbacks) {
    if (hwnd_ && owner_ == owner) return true;
    Destroy();
    if (!owner || !IsWindow(owner)) return false;
    owner_ = owner;
    callbacks_ = std::move(callbacks);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!GetClassInfoExW(wc.hInstance, kClass, &wc) && !RegisterClassExW(&wc)) return false;
    const UINT dpi = compat::WindowDpi(owner);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kClass,
        l10n::Pick(L"状态中心", L"Status center"), WS_POPUP | WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(464, dpi, 96), MulDiv(520, dpi, 96),
        owner, nullptr, wc.hInstance, this);
    return hwnd_ != nullptr;
}

void StatusCenterWindow::Destroy() {
    if (hwnd_) { HideComposedDialog(hwnd_); DestroyWindow(hwnd_); }
    hwnd_ = nullptr;
    owner_ = nullptr;
    hover_ = pressed_ = 0;
}

void StatusCenterWindow::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (hwnd_) {
        backdrop_ = ApplyBackdrop(hwnd_, dark_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void StatusCenterWindow::Update(const std::vector<app::StatusTaskRecord>& records) {
    // An update must not retarget a mouse press or a keyboard focus to a new row.
    const auto key_for = [&](int control) -> std::optional<app::StatusTaskKey> {
        if (control >= 3 && static_cast<size_t>((control - 3) / 2) < records_.size()) return records_[(control - 3) / 2].key;
        return std::nullopt;
    };
    const auto focused = key_for(focus_);
    const auto pressed = key_for(pressed_);
    records_ = records;
    const auto rebase = [&](int control, const std::optional<app::StatusTaskKey>& key) {
        if (control < 3) return control;
        for (size_t i = 0; i < records_.size(); ++i)
            if (key && records_[i].key == *key) return 3 + static_cast<int>(i) * 2 + (control - 3) % 2;
        return 0;
    };
    focus_ = rebase(focus_, focused);
    pressed_ = rebase(pressed_, pressed);
    if (!Enabled(focus_)) focus_ = 1;
    if (!Enabled(pressed_)) pressed_ = 0;
    if (!pressed_ && GetCapture() == hwnd_) ReleaseCapture();
    scroll_ = std::clamp(scroll_, 0.0f, MaxScroll());
    POINT cursor{};
    hover_ = visible() && GetCursorPos(&cursor) && ScreenToClient(hwnd_, &cursor)
        ? HitTest(static_cast<float>(cursor.x), static_cast<float>(cursor.y)) : 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void StatusCenterWindow::Show(POINT anchor) {
    if (!hwnd_) return;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const RECT work = monitor.rcWork;
    const int width = (std::min)(MulDiv(464, compat::WindowDpi(owner_), 96), static_cast<int>(work.right - work.left));
    const int height = (std::min)(MulDiv(520, compat::WindowDpi(owner_), 96), static_cast<int>(work.bottom - work.top));
    const int x = std::clamp(static_cast<int>(anchor.x) - width, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    const int y = std::clamp(static_cast<int>(anchor.y), static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    SetWindowPos(hwnd_, HWND_TOP, x, y, width, height, SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}

bool StatusCenterWindow::visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

float StatusCenterWindow::MaxScroll() const {
    const float height = static_cast<float>(compositor_.Height()) / (std::max)(scale_, 0.001f);
    return (std::max)(0.0f, static_cast<float>(records_.size()) * kRow - (height - kHeader - 12.0f));
}

void StatusCenterWindow::Scroll(float amount) {
    const float next = std::clamp(scroll_ + amount, 0.0f, MaxScroll());
    if (next != scroll_) { scroll_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
}

D2D1_RECT_F StatusCenterWindow::ButtonRect(int control) const {
    const float width = static_cast<float>(compositor_.Width()) / (std::max)(scale_, 0.001f);
    if (control == 1) return DipRect(scale_, width - 42, 8, 30, 30);
    if (control == 2) return DipRect(scale_, width - 148, 44, 132, 28);
    if (control < 3) return {};
    const float y = kHeader + static_cast<float>((control - 3) / 2) * kRow - scroll_ + 92;
    return DipRect(scale_, (control - 3) % 2 == 0 ? width - 94 : 22, y, (control - 3) % 2 == 0 ? 68.0f : 88.0f, 28);
}

bool StatusCenterWindow::Enabled(int control) const {
    if (control == 1) return true;
    if (control == 2) return std::any_of(records_.begin(), records_.end(), [](const auto& record) { return app::StatusTaskTerminal(record.state); });
    if (control < 3 || static_cast<size_t>((control - 3) / 2) >= records_.size()) return false;
    const auto& record = records_[(control - 3) / 2];
    return (control - 3) % 2 == 0 ? record.cancellable
        : record.key.source == app::StatusTaskSource::Operation && !app::StatusTaskTerminal(record.state);
}

int StatusCenterWindow::HitTest(float x, float y) const {
    if (ContainsRect(ButtonRect(1), x, y)) return 1;
    if (Enabled(2) && ContainsRect(ButtonRect(2), x, y)) return 2;
    if (y < kHeader * scale_ || y >= static_cast<float>(compositor_.Height()) - 12 * scale_) return 0;
    for (int control = 3; control < 3 + static_cast<int>(records_.size()) * 2; ++control)
        if (Enabled(control) && ContainsRect(ButtonRect(control), x, y)) return control;
    return 0;
}

void StatusCenterWindow::Activate(int control) {
    if (!Enabled(control)) return;
    if (control == 1) { ShowWindow(hwnd_, SW_HIDE); return; }
    if (control == 2) { if (callbacks_.clear_completed) callbacks_.clear_completed(); return; }
    const auto key = records_[(control - 3) / 2].key;
    if ((control - 3) % 2 == 0) { if (callbacks_.cancel) callbacks_.cancel(key); }
    else if (callbacks_.operation_details) callbacks_.operation_details(key.id);
}

void StatusCenterWindow::FocusNext(bool backwards) {
    const int controls = 3 + static_cast<int>(records_.size()) * 2;
    for (int attempt = 0; attempt < controls; ++attempt) {
        focus_ = (focus_ + (backwards ? controls - 1 : 1)) % controls;
        if (Enabled(focus_)) break;
    }
    if (focus_ >= 3) {
        const float top = static_cast<float>((focus_ - 3) / 2) * kRow;
        const float view = static_cast<float>(compositor_.Height()) / scale_ - kHeader - 12;
        if (top < scroll_) scroll_ = top;
        else if (top + kRow > scroll_ + view) scroll_ = top + kRow - view;
        scroll_ = std::clamp(scroll_, 0.0f, MaxScroll());
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void StatusCenterWindow::Render(bool present) {
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
    }
    if (!compositor_.Dc()) return;
    const bool contrast = IsHighContrast();
    const Theme theme = contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    BeginSurface(compositor_, painter_, theme, dark_, contrast, backdrop_, scale_);
    const float width = static_cast<float>(compositor_.Width()) / scale_;
    const float height = static_cast<float>(compositor_.Height()) / scale_;
    const auto text = [&](const std::wstring& value, float x, float y, float w, float h, bool heading = false, bool error = false) {
        painter_.DrawText(value, DipRect(scale_, x, y, w, h), heading ? compositor_.HeaderFormat() : compositor_.SmallFormat(),
            error ? theme.danger : heading ? theme.text : theme.text_secondary);
    };
    const auto button = [&](int control, const std::wstring& label, const wchar_t* glyph = L"") {
        fluent::ButtonSpec spec;
        spec.bounds = ButtonRect(control);
        spec.text = label;
        spec.glyph = glyph;
        spec.icon_only = control == 1;
        spec.kind = control == 1 ? fluent::ButtonKind::Transparent : fluent::ButtonKind::Standard;
        spec.state.enabled = Enabled(control);
        spec.state.hovered = hover_ == control;
        spec.state.pressed = pressed_ == control;
        painter_.DrawButton(spec);
        if (focus_ == control && GetFocus() == hwnd_) painter_.DrawFocusRing(spec.bounds, 4 * scale_);
    };
    text(l10n::Pick(L"状态中心", L"Status center"), 18, 10, width - 70, 26, true);
    const auto active = std::count_if(records_.begin(), records_.end(), [](const auto& record) { return !app::StatusTaskTerminal(record.state); });
    text(std::to_wstring(active) + l10n::Pick(L" 个进行中", L" active"), 18, 44, width - 176, 28);
    button(1, L"", L"\xE711");
    button(2, l10n::Pick(L"清除完成记录", L"Clear history"));
    if (records_.empty()) text(l10n::Pick(L"暂无任务记录", L"No tasks yet"), 18, 116, width - 36, 32);
    auto* dc = compositor_.Dc();
    dc->PushAxisAlignedClip(DipRect(scale_, 0, kHeader, width, (std::max)(0.0f, height - kHeader - 12)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < records_.size(); ++i) {
        const float y = kHeader + static_cast<float>(i) * kRow - scroll_;
        if (y + kRow < kHeader || y > height - 12) continue;
        const auto& record = records_[i];
        const bool error = record.state == app::StatusTaskState::Failed || record.state == app::StatusTaskState::CompletedWithErrors;
        const auto bounds = DipRect(scale_, 12, y + 2, width - 24, kRow - 8);
        painter_.FillRoundedRect(bounds, 8 * scale_, theme.surface_card);
        painter_.StrokeRoundedRect(bounds, 8 * scale_, theme.stroke_divider);
        std::wstring title = TaskName(record) + L" · " + StateName(record.state);
        if (!app::StatusTaskTerminal(record.state) && record.progress >= 0) title += L" " + std::to_wstring(static_cast<int>(record.progress * 100)) + L"%";
        text(title, 22, y + 8, width - 44, 23, true, error);
        std::wstring source = record.source;
        if (record.item_count > 1) source += L" · " + std::to_wstring(record.item_count) + l10n::Pick(L" 项", L" items");
        text(source, 22, y + 32, width - 44, 19);
        text(record.detail.empty() ? record.destination : record.detail, 22, y + 53, width - 44, 23, false, error);
        if (!app::StatusTaskTerminal(record.state)) {
            fluent::ProgressSpec progress;
            progress.bounds = DipRect(scale_, 22, y + 82, width - 44, 3);
            progress.value = static_cast<float>((std::max)(0.0, record.progress));
            progress.indeterminate = record.progress < 0;
            progress.paused = record.state == app::StatusTaskState::Paused || record.state == app::StatusTaskState::Waiting;
            painter_.DrawProgressBar(progress);
        }
        const int cancel = 3 + static_cast<int>(i) * 2;
        if (Enabled(cancel)) button(cancel, l10n::Get(l10n::StringId::Cancel));
        if (Enabled(cancel + 1)) button(cancel + 1, l10n::Pick(L"查看详情", L"Details"));
    }
    dc->PopAxisAlignedClip();
    if (MaxScroll() > 0) {
        fluent::ScrollbarSpec scrollbar;
        scrollbar.viewport = DipRect(scale_, 0, kHeader, width, height - kHeader - 12);
        scrollbar.content_extent = static_cast<float>(records_.size()) * kRow * scale_;
        scrollbar.viewport_extent = (height - kHeader - 12) * scale_;
        scrollbar.offset = scroll_ * scale_;
        painter_.DrawScrollbar(scrollbar);
    }
    if (present) EndSurface(compositor_);
    else {
        const HRESULT result = compositor_.Dc()->EndDraw();
        if (FAILED(result)) compositor_.NotifyDeviceLost(result);
    }
}

LRESULT CALLBACK StatusCenterWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<StatusCenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<StatusCenterWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
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

LRESULT StatusCenterWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
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
    case WM_NCHITTEST:
        return BorderlessHitTest(hwnd_, lparam, 42 * scale_, ButtonRect(1));
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {static_cast<LONG>(320 * scale_), static_cast<LONG>(240 * scale_)};
        return 0;
    }
    case WM_SIZE:
        if (LOWORD(lparam) && HIWORD(lparam)) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
        Scroll(0);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int hit = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (hover_ != hit) { hover_ = hit; InvalidateRect(hwnd_, nullptr, FALSE); }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE: hover_ = 0; InvalidateRect(hwnd_, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        pressed_ = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (pressed_) { focus_ = pressed_; SetCapture(hwnd_); SetFocus(hwnd_); InvalidateRect(hwnd_, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        const int pressed = pressed_;
        pressed_ = 0;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (pressed && pressed == HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)))) Activate(pressed);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED: pressed_ = 0; return 0;
    case WM_MOUSEWHEEL: Scroll(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 72.0f); return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) ShowWindow(hwnd_, SW_HIDE);
        else if (wparam == VK_TAB) FocusNext((GetKeyState(VK_SHIFT) & 0x8000) != 0);
        else if (wparam == VK_RETURN || wparam == VK_SPACE) Activate(focus_);
        else if (wparam == VK_DOWN || wparam == VK_NEXT) Scroll(wparam == VK_DOWN ? kRow : kRow * 3);
        else if (wparam == VK_UP || wparam == VK_PRIOR) Scroll(wparam == VK_UP ? -kRow : -kRow * 3);
        return 0;
    case WM_ACTIVATE: if (LOWORD(wparam) == WA_INACTIVE) ShowWindow(hwnd_, SW_HIDE); return 0;
    case WM_CLOSE: ShowWindow(hwnd_, SW_HIDE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: compositor_.Shutdown(); return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace pulse::ui
