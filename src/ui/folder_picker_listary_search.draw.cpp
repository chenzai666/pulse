#include "../common/windows_compat.h"
#include "folder_picker_listary_search.h"
#include "name_highlight.h"
#include "shell_icons.h"
#include "typography.h"
#include "window_helpers.h"
#include <algorithm>

namespace pulse::ui {
namespace {
constexpr float kRowDip = 44.0f;
std::wstring Fit(Compositor& compositor, std::wstring_view text, float width) {
    return FitTextEnd(std::wstring(text), std::max(0.0f, width), [&](std::wstring_view value) {
        return typography::MeasureLine(&compositor, compositor.TextFormat(), value);
    });
}

void DrawMatchedName(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
                     const std::wstring& name, std::wstring_view query,
                     const D2D1_RECT_F& bounds, float scale) {
    const float width = bounds.right - bounds.left;
    if (width <= 0) return;
    // The shared highlight adds two DIP on either side of a matched span.
    // Reserve that advance before fitting so a highlighted suffix cannot run
    // over the ellipsis or the next row/column.
    float budget = width;
    for (int pass = 0; pass < 2; ++pass) {
        const auto shown = Fit(compositor, name, budget);
        const auto ranges = ListaryNameMatches(name, query, shown);
        const float padding = static_cast<float>(ranges.size()) * 2.0f * kNameHighlightPaddingDip * scale;
        const float fitted_budget = std::min(budget, std::max(0.0f, width - padding));
        if (fitted_budget == budget) break;
        budget = fitted_budget;
    }
    const auto shown = Fit(compositor, name, budget);
    std::vector<NameMatchRange> visible;
    for (const auto& range : ListaryNameMatches(name, query, shown))
        visible.push_back({range.start, range.length});
    if (visible.empty() || !compositor.Dc() || !compositor.DwriteFactory()) {
        painter.DrawText(shown, bounds, compositor.TextFormat(), theme.text);
        return;
    }
    ComPtr<IDWriteTextLayout> layout;
    ComPtr<ID2D1SolidColorBrush> ink;
    const auto rect = typography::SnapVerticalBounds(bounds);
    if (FAILED(compositor.DwriteFactory()->CreateTextLayout(shown.c_str(),
            static_cast<UINT32>(shown.size()), compositor.TextFormat(), width,
            rect.bottom - rect.top, &layout)) ||
        FAILED(compositor.Dc()->CreateSolidColorBrush(theme.text, &ink))) {
        painter.DrawText(shown, bounds, compositor.TextFormat(), theme.text);
        return;
    }
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // Shape the whole visible filename once. This is the same highlight policy
    // as Pulse's file list, with query spans clipped to the visible prefix.
    DrawNameHighlightBackground(&compositor, layout.get(), {rect.left, rect.top},
                                 rect, visible, theme, scale);
    compositor.Dc()->DrawTextLayout({rect.left, rect.top}, layout.get(), ink.get(),
                                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
} // namespace

void PickerListarySearch::PrefetchIcons(ShellIconCache& icons) const {
    if (!PopupVisible()) return;
    for (const auto& item : items_)
        icons.Prefetch(item.path, ListaryResultName(item), item.directory,
            item.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, 20.0f * scale_);
}

void PickerListarySearch::Draw(Compositor& compositor, fluent::Painter& painter,
                              const Theme& theme, bool enabled, ShellIconCache& icons) const {
    painter.FillRoundedRect(band_, 0, theme.surface_sheet);
    painter.FillRoundedRect(D2D1::RectF(0, band_.top, band_.right, band_.top + 1),
                            0, theme.stroke_divider);
    painter.DrawText(L"Listary", D2D1::RectF(16.0f * scale_, input_.top,
                     input_.left - 8.0f * scale_, input_.bottom), compositor.TextFormat(), theme.text);
    fluent::ControlState state{};
    state.enabled = enabled;
    state.focused = focused_;
    painter.DrawTextFieldFrame(input_, state, true);
    if (!query_.empty() && !status_.empty()) {
        // Status lives inside the dedicated band, including errors and loading;
        // it never covers the file list, filename or confirmation controls.
        const auto hint = D2D1::RectF(input_.left, input_.bottom,
                                     input_.right, band_.bottom - 2.0f * scale_);
        painter.DrawText(Fit(compositor, status_, hint.right - hint.left), hint,
                         compositor.TextFormat(), theme.text_secondary);
    }
    if (!PopupVisible()) return;
    const auto panel = PanelBounds();
    painter.FillRoundedRect(panel, theme.radius_control * scale_, theme.surface_flyout);
    painter.StrokeRoundedRect(panel, theme.radius_control * scale_, theme.stroke_card);
    const int visible = std::max(1, static_cast<int>((panel.bottom - panel.top) / (kRowDip * scale_)));
    const int first = std::max(0, selected_ - visible + 1);
    for (int offset = 0; offset < visible && first + offset < static_cast<int>(items_.size()); ++offset) {
        const int index = first + offset;
        const auto& item = items_[static_cast<size_t>(index)];
        const float top = panel.top + static_cast<float>(offset) * kRowDip * scale_;
        const auto row = D2D1::RectF(panel.left + 2, top, panel.right - 2, top + kRowDip * scale_);
        if (index == selected_) painter.FillRoundedRect(row, 4.0f * scale_, theme.fill_selected);
        const auto icon = D2D1::RectF(row.left + 10.0f * scale_, top + 12.0f * scale_,
                                     row.left + 30.0f * scale_, top + 32.0f * scale_);
        const auto display_name = ListaryResultName(item);
        // CachedBitmapFor only uploads pixels already converted by the Shell
        // worker. Never call Draw/BitmapFor here: they can synchronously extract
        // and convert icons while a D2D frame is open.
        auto* bitmap = icons.CachedBitmapFor(item.path, display_name, item.directory,
            item.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, 20.0f * scale_);
        if (bitmap && compositor.Dc()) {
            compositor.Dc()->DrawBitmap(bitmap, icon, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        } else {
            painter.DrawGlyph(item.directory ? L"\xE8B7" : L"\xE8A5", icon,
                              item.directory ? theme.icon_folder : theme.text_secondary);
        }
        const auto name = D2D1::RectF(icon.right + 8.0f * scale_, top,
                                      row.right - 10.0f * scale_, top + 23.0f * scale_);
        const auto path = D2D1::RectF(name.left, name.bottom, name.right, row.bottom);
        DrawMatchedName(compositor, painter, theme, display_name, query_, name, scale_);
        painter.DrawText(Fit(compositor, ListaryResultParent(item.path), path.right - path.left), path,
                         compositor.TextFormat(), theme.text_secondary);
    }
}

} // namespace pulse::ui
