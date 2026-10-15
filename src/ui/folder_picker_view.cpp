#include "../common/windows_compat.h"
#include "folder_picker_view.h"

#include "../common/localization.h"
#include "typography.h"
#include "window_helpers.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr float kFieldH = 34.0f;
constexpr float kButtonMinW = 120.0f;
constexpr float kSidePad = 16.0f;

float Dip(float scale, float value) { return value * scale; }

fluent::ControlState StateFor(const FolderPickerChrome& c, int id, bool enabled = true) {
    fluent::ControlState state{};
    state.enabled = enabled;
    state.hovered = enabled && c.hover == id;
    state.pressed = enabled && c.pressed == id;
    state.keyboard_focus = c.show_focus && c.focus == id;
    return state;
}

std::wstring FitEnd(Compositor& compositor, IDWriteTextFormat* format, const std::wstring& text,
                    float width) {
    return FitTextEnd(text, width, [&](std::wstring_view s) {
        return typography::MeasureLine(&compositor, format, s);
    });
}

} // namespace

float PickerFooterDip(PickerMode mode) {
    return mode == PickerMode::Folder ? 60.0f : 104.0f;
}

FolderPickerChromeLayout LayoutPickerChrome(float width, float height,
                                            const FolderPickerChrome& chrome,
                                            const fluent::Painter& painter, float scale) {
    FolderPickerChromeLayout l;
    l.scale = scale;
    l.width = width;
    l.height = height;
    l.title = D2D1::RectF(0, 0, width, Dip(scale, kPickerTitleDip));
    l.close = D2D1::RectF(width - Dip(scale, 46.0f), 0, width, l.title.bottom);
    l.footer = D2D1::RectF(0, height - Dip(scale, PickerFooterDip(chrome.mode)), width, height);

    const float left = Dip(scale, kSidePad);
    const float right = width - Dip(scale, kSidePad);
    const float btn_h = painter.MeasureButtonHeight();
    // The button row is the last one; files get the name / type row above it.
    const float button_band = Dip(scale, 60.0f);
    const float by = height - button_band + (button_band - btn_h) * 0.5f;
    const float primary_w = std::max(painter.MeasureButtonWidth(chrome.primary_text),
                                     Dip(scale, kButtonMinW));
    const float cancel_w = std::max(painter.MeasureButtonWidth(chrome.cancel_text),
                                    Dip(scale, kButtonMinW));
    l.primary = D2D1::RectF(right - primary_w, by, right, by + btn_h);
    l.cancel = D2D1::RectF(l.primary.left - Dip(scale, 8.0f) - cancel_w, by,
                           l.primary.left - Dip(scale, 8.0f), by + btn_h);
    l.summary = D2D1::RectF(left, height - button_band, l.cancel.left - Dip(scale, 16.0f), height);

    if (chrome.mode != PickerMode::Folder) {
        const float fy = l.footer.top + Dip(scale, 10.0f);
        const float fh = Dip(scale, kFieldH);
        const float label_w = painter.MeasureButtonWidth(chrome.filename_label) - Dip(scale, 16.0f);
        l.filename_label = D2D1::RectF(left, fy, left + std::max(0.0f, label_w), fy + fh);
        // The type list keeps a readable width; the name takes the rest.
        const float filter_w = std::clamp((right - left) * 0.3f, Dip(scale, 180.0f), Dip(scale, 300.0f));
        l.filter = D2D1::RectF(right - filter_w, fy, right, fy + fh);
        l.filename = D2D1::RectF(l.filename_label.right + Dip(scale, 8.0f), fy,
                                 l.filter.left - Dip(scale, 8.0f), fy + fh);
    }
    return l;
}

int HitTestPickerChrome(const FolderPickerChromeLayout& l, const FolderPickerChrome& c,
                        float x, float y) {
    if (ContainsRect(l.close, x, y)) return kPickClose;
    if (ContainsRect(l.primary, x, y)) return c.primary_enabled ? kPickPrimary : kPickNone;
    if (ContainsRect(l.cancel, x, y)) return kPickCancel;
    if (ContainsRect(l.filename, x, y) || ContainsRect(l.filename_label, x, y)) return kPickFilename;
    if (ContainsRect(l.filter, x, y)) return kPickFilter;
    return kPickNone;
}

void DrawPickerChrome(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                      const FolderPickerChrome& c, const FolderPickerChromeLayout& l,
                      bool high_contrast) {
    const float scale = l.scale;
    const float radius = Dip(scale, theme.radius_control);

    // Title strip: what the caller asked for, and the close button.
    const wchar_t* glyph = c.mode == PickerMode::Image ? L"\xEB9F"
                         : c.mode == PickerMode::Save ? L"\xE74E"
                         : c.mode == PickerMode::File ? L"\xE8A5" : L"\xE8B7";
    painter.DrawGlyph(glyph, D2D1::RectF(Dip(scale, 14.0f), 0, Dip(scale, 38.0f), l.title.bottom),
                      high_contrast ? theme.text : theme.accent);
    painter.DrawText(FitEnd(compositor, compositor.HeaderFormat(), c.title,
                            l.close.left - Dip(scale, 54.0f)),
                     D2D1::RectF(Dip(scale, 46.0f), 0, l.close.left - Dip(scale, 8.0f), l.title.bottom),
                     compositor.HeaderFormat(), theme.text);
    fluent::ControlState close_state{};
    close_state.hovered = c.hover == kPickClose;
    close_state.pressed = c.pressed == kPickClose;
    painter.DrawTitleBarButton(l.close, fluent::TitleBarButtonRole::Close, {}, close_state);

    // The shared sheet runs to the bottom edge; a hairline separates the footer
    // from the panes like the main window's status bar.
    painter.FillRoundedRect(D2D1::RectF(l.footer.left, l.footer.top, l.footer.right,
                                        l.footer.top + 1.0f),
                            0, theme.stroke_divider);

    // File name and file type.
    if (l.filename.right > l.filename.left) {
        painter.DrawText(c.filename_label, l.filename_label, compositor.TextFormat(), theme.text);
        fluent::ControlState field = StateFor(c, kPickFilename);
        field.focused = c.filename_focused;
        painter.DrawTextFieldFrame(l.filename, field, c.hosted_edit);
        if (!c.hosted_edit && !c.filename_text.empty()) {
            const D2D1_RECT_F inner = D2D1::RectF(l.filename.left + Dip(scale, 10.0f), l.filename.top,
                                                  l.filename.right - Dip(scale, 10.0f), l.filename.bottom);
            painter.DrawText(FitEnd(compositor, compositor.TextFormat(), c.filename_text,
                                    inner.right - inner.left),
                             inner, compositor.TextFormat(), theme.text);
        }
        fluent::ButtonSpec filter{l.filter, c.filter_text, {}, fluent::ButtonKind::Standard,
                                 StateFor(c, kPickFilter)};
        filter.drop_down = true;
        filter.chevron_turn = c.filter_turn;
        painter.DrawButton(filter);
    }

    // What will be picked (or why not), then the buttons.
    const bool notice = !c.notice.empty();
    const std::wstring& line = notice ? c.notice : c.summary;
    if (!line.empty()) {
        painter.DrawText(FitEnd(compositor, compositor.TextFormat(), line,
                                l.summary.right - l.summary.left),
                         l.summary, compositor.TextFormat(),
                         notice ? theme.danger : theme.text_secondary);
    }
    painter.DrawButton({l.cancel, c.cancel_text, {}, fluent::ButtonKind::Standard,
                        StateFor(c, kPickCancel)});
    const fluent::ControlState primary = StateFor(c, kPickPrimary, c.primary_enabled);
    painter.DrawButton({l.primary, c.primary_text, {}, fluent::ButtonKind::Primary, primary});
    // The shared accent ring vanishes against a filled button; add the Fluent
    // outer ring in the text colour.
    if (primary.keyboard_focus) {
        const float gap = Dip(scale, 3.0f);
        painter.StrokeRoundedRect(D2D1::RectF(l.primary.left - gap, l.primary.top - gap,
                                              l.primary.right + gap, l.primary.bottom + gap),
                                  radius + gap, theme.text, Dip(scale, 1.5f));
    }
}

} // namespace pulse::ui
