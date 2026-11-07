#include "../common/windows_compat.h"
#include "folder_picker_listary_search.h"
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
} // namespace
void PickerListarySearch::Draw(Compositor& compositor, fluent::Painter& painter,
                              const Theme& theme, bool enabled) const {
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
        const auto icon = D2D1::RectF(row.left + 8.0f * scale_, top, row.left + 34.0f * scale_, row.bottom);
        painter.DrawGlyph(item.directory ? L"\xE8B7" : L"\xE8A5", icon,
                          item.directory ? theme.icon_folder : theme.text_secondary);
        const auto name = D2D1::RectF(icon.right + 4.0f * scale_, top,
                                      row.right - 10.0f * scale_, top + 23.0f * scale_);
        const auto path = D2D1::RectF(name.left, name.bottom, name.right, row.bottom);
        painter.DrawText(Fit(compositor, item.name.empty() ? item.path : item.name,
                             name.right - name.left), name, compositor.TextFormat(), theme.text);
        painter.DrawText(Fit(compositor, item.path, path.right - path.left), path,
                         compositor.TextFormat(), theme.text_secondary);
    }
}

} // namespace pulse::ui
