#include "recycle_toolbar.h"
#include "ui_renderer_internal.h"

namespace pulse::ui {
namespace {
constexpr l10n::StringId kLabels[] = {
    l10n::StringId::EmptyRecycleBin, l10n::StringId::RestoreAll, l10n::StringId::Restore
};

RecycleToolbarLayout Layout(Compositor* compositor, const ToolbarLayout& toolbar, float width, float scale) {
    std::array<float, 3> text_widths{};
    for (size_t i = 0; i < text_widths.size(); ++i)
        text_widths[i] = MeasureTextWidth(compositor->DwriteFactory(), compositor->TextFormat(), l10n::Get(kLabels[i]));
    return MakeRecycleToolbarLayout({toolbar.create.left, toolbar.create.top,
        (std::max)(toolbar.create.left, width - 12 * scale), toolbar.create.bottom},
        scale, text_widths);
}

}

bool RecycleToolbarActionEnabled(const PaneViewModel& pane, int action) {
    if (!pane.is_recycle || pane.loading || action < 0 || action > 2) return false;
    if (action == 2) return pane.selected_count > 0;
    return pane.snapshot ? !pane.snapshot->empty() : !pane.entries.empty();
}

bool MainRenderer::DrawRecycleToolbar(const WindowViewModel& vm, const ToolbarLayout& toolbar, float width) {
    if (!vm.pane.is_recycle) return false;
    const auto layout = Layout(compositor_, toolbar, width, scale_);
    constexpr const wchar_t* glyphs[] = {L"\xE74D", L"\xE777", L"\xE7A7"};
    for (int i = 0; i < 3; ++i) {
        if (layout.buttons[i].right <= layout.buttons[i].left) continue;
        fluent::ButtonSpec button;
        button.bounds = layout.buttons[i];
        button.glyph = glyphs[i];
        button.text = layout.icon_only ? std::wstring_view{} : std::wstring_view{l10n::Get(kLabels[i])};
        button.icon_only = layout.icon_only;
        button.kind = fluent::ButtonKind::Transparent;
        button.bordered = false;
        button.state.enabled = RecycleToolbarActionEnabled(vm.pane, i);
        button.state.hovered = button.state.enabled && IsHovered(vm, HitTestResult::RecycleToolbarAction, i);
        painter_.DrawButton(button);
    }
    return true;
}

int MainRenderer::HitRecycleToolbar(const WindowViewModel& vm, const ToolbarLayout& toolbar,
                                  float width, float x, float y) const {
    if (!vm.pane.is_recycle) return -1;
    const auto layout = Layout(compositor_, toolbar, width, scale_);
    for (int i = 0; i < 3; ++i) {
        const auto& button = layout.buttons[i];
        if (RecycleToolbarActionEnabled(vm.pane, i) && x >= button.left && x < button.right && y >= button.top && y < button.bottom) return i;
    }
    return -1;
}
}
