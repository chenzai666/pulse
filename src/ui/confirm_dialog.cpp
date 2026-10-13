#include "../common/windows_compat.h"
#include "confirm_dialog.h"

#include "confirm_dialog_view.h"
#include "window_helpers.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace pulse::ui {
namespace {

constexpr wchar_t kConfirmClass[] = L"PulseConfirmWindow";

bool CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (memory) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) GlobalFree(memory);
    }
    CloseClipboard();
    return ok;
}

class ConfirmWindow {
public:
    ConfirmDialogResult Show(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                             D2D1_COLOR_F accent) {
        owner_ = owner;
        base_spec_ = NormalizeConfirmSpec(spec);
        spec_ = ConfirmSpecForCheckbox(base_spec_, base_spec_.checkbox.checked);
        dark_ = dark;
        accent_ = accent;
        choice_ = ConfirmChoice::Cancel;
        visual_.focus = ConfirmControlFor(spec_.default_choice, HasSecondary());
        // Any other default is not what the colours suggest, so show it at once.
        visual_.show_focus = visual_.focus != kConfirmPrimary;
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow()))
               / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConfirmClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kConfirmClass, &wc)) RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kConfirmClass,
            spec_.title.c_str(), WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(420.0f * scale_),
            static_cast<int>(200.0f * scale_), owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return {};
        RECT placed{};
        GetWindowRect(hwnd_, &placed);
        CenterOwnedWindow(hwnd_, owner_, placed.right - placed.left,
                          placed.bottom - placed.top);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);

        MSG message{};
        while (!done_) {
            const BOOL got = GetMessageW(&message, nullptr, 0, 0);
            if (got <= 0) {
                if (got == 0) PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            RedirectStrayModalKey(message, hwnd_);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        return {choice_, spec_.checkbox.checked};
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConfirmWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConfirmWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    bool HasSecondary() const { return !spec_.secondary_text.empty(); }

    void SizeToContent() {
        if (!hwnd_ || !compositor_.DwriteFactory()) return;
        formats_.Create(compositor_.DwriteFactory(), scale_);
        layout_ = LayoutConfirmDialog(spec_, painter_, compositor_.DwriteFactory(), formats_,
                                      scale_);
        const int width = static_cast<int>(std::ceil(layout_.width));
        const int height = static_cast<int>(std::ceil(layout_.height));
        sizing_ = true;
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        sizing_ = false;
        compositor_.Resize(width, height);
    }

    void Complete(ConfirmChoice choice) {
        choice_ = choice;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int id) {
        if (id == kConfirmPrimary) Complete(ConfirmChoice::Confirm);
        else if (id == kConfirmSecondary) Complete(ConfirmChoice::Secondary);
        else if (id == kConfirmCancel || id == kConfirmClose) Complete(ConfirmChoice::Cancel);
        else if (id == kConfirmCheckbox && !base_spec_.checkbox.text.empty()) {
            spec_ = ConfirmSpecForCheckbox(base_spec_, !spec_.checkbox.checked);
            visual_.focus = kConfirmCheckbox;
            visual_.show_focus = true;
            SizeToContent();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void MoveFocus(int direction) {
        const std::vector<int> order = ConfirmFocusOrder(spec_);
        auto it = std::find(order.begin(), order.end(), visual_.focus);
        int index = it == order.end() ? 0 : static_cast<int>(it - order.begin());
        const int count = static_cast<int>(order.size());
        index = (index + direction + count) % count;
        visual_.focus = order[static_cast<size_t>(index)];
        visual_.show_focus = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);
        DrawConfirmDialog(compositor_, painter_, theme, spec_, layout_, formats_, visual_,
                          dark_, high_contrast);
        EndSurface(compositor_);
    }

    int Hit(LPARAM lparam) const {
        return HitTestConfirmDialog(layout_, HasSecondary(),
                                    static_cast<float>(GET_X_LPARAM(lparam)),
                                    static_cast<float>(GET_Y_LPARAM(lparam)));
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(pulse::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            SizeToContent();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            // The size follows the content: edges are not resize handles.
            const LRESULT hit = BorderlessHitTest(hwnd_, lparam, layout_.title_bar,
                                                  layout_.close);
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT ? HTCLIENT : hit;
        }
        case WM_SIZE:
            if (!sizing_ && compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam)) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            }
            SizeToContent();
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(lparam);
            if (next != visual_.hover) {
                visual_.hover = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            visual_.hover = kConfirmNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            visual_.pressed = Hit(lparam);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(lparam);
            const int pressed = visual_.pressed;
            visual_.pressed = kConfirmNone;
            ReleaseCapture();
            if (hit == pressed) Activate(hit);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wparam) {
            case VK_ESCAPE: Complete(ConfirmChoice::Cancel); return 0;
            case VK_TAB: MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); return 0;
            case VK_LEFT: MoveFocus(-1); return 0;
            case VK_RIGHT: MoveFocus(1); return 0;
            case VK_RETURN:
            case VK_SPACE: Activate(visual_.focus); return 0;
            case 'C':
                if (GetKeyState(VK_CONTROL) & 0x8000) {
                    CopyTextToClipboard(hwnd_, ConfirmClipboardText(spec_));
                    return 0;
                }
                break;
            }
            break;
        case WM_CLOSE:
            Complete(ConfirmChoice::Cancel);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    ConfirmDialogFormats formats_;
    ConfirmDialogLayout layout_;
    ConfirmDialogVisual visual_;
    ConfirmDialogSpec base_spec_;
    ConfirmDialogSpec spec_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    ConfirmChoice choice_ = ConfirmChoice::Cancel;
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool sizing_ = false;
};

} // namespace

ConfirmDialogResult ShowConfirmDialogWithOptions(HWND owner, const ConfirmDialogSpec& spec,
                                                 bool dark, D2D1_COLOR_F accent) {
    ConfirmWindow window;
    return window.Show(owner, spec, dark, accent);
}

ConfirmChoice ShowConfirmDialogEx(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                                  D2D1_COLOR_F accent) {
    return ShowConfirmDialogWithOptions(owner, spec, dark, accent).choice;
}

bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent) {
    return ShowConfirmDialogEx(owner, spec, dark, accent) == ConfirmChoice::Confirm;
}

} // namespace pulse::ui
