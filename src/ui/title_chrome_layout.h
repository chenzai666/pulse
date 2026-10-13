#pragma once

#include <algorithm>

namespace pulse::ui {

struct TitleChrome {
    float chrome_left = 0.0f;
    float status_left = 0.0f, status_w = 0.0f;
    float settings_left = 0.0f, settings_w = 0.0f;
    float theme_left = 0.0f, theme_w = 0.0f;
    float group_left = 0.0f, group_right = 0.0f;
    float cmd_left = 0.0f, cmd_w = 0.0f, cmd_top = 0.0f, cmd_h = 0.0f;
};

inline TitleChrome MakeTitleChrome(float window_w, float scale, float title_h, float content_left = 0.0f) {
    TitleChrome c;
    c.chrome_left = window_w - 46.0f * scale * 3.0f;
    c.theme_w = c.settings_w = 36.0f * scale;
    const float pad = 2.0f * scale;
    c.group_right = c.chrome_left - 14.0f * scale;
    c.theme_left = c.group_right - pad - c.theme_w;
    c.settings_left = c.theme_left - scale - c.settings_w;
    // The same action remains reachable in ToolbarMore on narrower windows.
    c.status_w = window_w >= 680.0f * scale && window_w - content_left >= 600.0f * scale ? 36.0f * scale : 0.0f;
    c.status_left = c.settings_left - (c.status_w > 0 ? scale + c.status_w : 0.0f);
    c.group_left = c.status_left - pad;
    c.cmd_left = c.group_left;
    c.cmd_top = 8.0f * scale;
    c.cmd_h = (std::max)(0.0f, title_h - c.cmd_top * 2.0f);
    return c;
}

} // namespace pulse::ui
