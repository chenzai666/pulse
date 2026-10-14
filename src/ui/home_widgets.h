#pragma once
#include <d2d1_1.h>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {
class Compositor;
struct Theme;
namespace fluent { class Painter; }

struct HomeWidgetItem {
    std::wstring title, detail, path, glyph;
    bool file = false;
    bool drive = false;
    bool pinned = false, danger = false;
    float used_ratio = 0;
    uint32_t color = 0;
    D2D1_COLOR_F icon_color{};
};
struct HomeWidget {
    std::wstring title, empty_text;
    std::vector<HomeWidgetItem> items;
    bool visible = true;
};
struct HomeWidgets {
    std::vector<HomeWidget> sections;
};
struct HomeWidgetHit {
    int index = -1;
    std::wstring path;
    bool file = false;
};

// 绘制和命中共用布局；输入仅为应用已缓存的数据，不在 UI 线程访问磁盘或网络。
float HomeWidgetsHeight(const HomeWidgets& widgets, float width, float scale);
HomeWidgetHit HitHomeWidgets(const HomeWidgets& widgets, const D2D1_RECT_F& bounds,
                            float scroll, float scale, float x, float y);
void DrawHomeWidgets(fluent::Painter& painter, Compositor& compositor, const Theme& theme,
                     const HomeWidgets& widgets, const D2D1_RECT_F& bounds,
                     float scroll, float scale, int hovered,
                     const std::function<bool(std::wstring_view,const D2D1_RECT_F&)>& draw_icon = {});
}
