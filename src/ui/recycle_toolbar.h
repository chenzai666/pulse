#pragma once
#include <array>
#include <algorithm>
#include <d2d1.h>

namespace pulse::ui {
struct PaneViewModel;
bool RecycleToolbarActionEnabled(const PaneViewModel& pane, int action);

struct RecycleToolbarLayout {
    std::array<D2D1_RECT_F, 3> buttons{};
    bool icon_only = false;
};

inline RecycleToolbarLayout MakeRecycleToolbarLayout(const D2D1_RECT_F& row, float scale,
                                                     const std::array<float, 3>& text_widths) {
    RecycleToolbarLayout result;
    const float available = (std::max)(0.0f, row.right - row.left);
    const float gap = 6.0f * scale;
    std::array<float, 3> widths{};
    float required = gap * 2.0f;
    for (size_t i = 0; i < widths.size(); ++i) {
        widths[i] = text_widths[i] + 44.0f * scale;
        required += widths[i];
    }
    result.icon_only = required > available;
    if (result.icon_only) widths.fill((std::max)(0.0f, (std::min)(32.0f * scale, (available - 2 * gap) / 3.0f)));
    float x = row.left;
    for (size_t i = 0; i < widths.size(); ++i) {
        if (widths[i] > 0) result.buttons[i] = {x, row.top, x + widths[i], row.bottom};
        x += widths[i] + gap;
    }
    return result;
}
}
