#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>

namespace pulse::ui {
inline float NavigationStep(float available, float scale) {
    return (available < 500 * scale ? 26.0f : 34.0f) * scale;
}
inline D2D1_RECT_F NavigationButtonAt(float x, float y, float scale, float step) {
    return {x, y, x + step - 4 * scale, y + 32 * scale};
}
struct ToolbarLayout {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, create{}, properties{};
    D2D1_RECT_F sort{}, filter{}, overflow{};
    D2D1_RECT_F group{};   // "Group" button / active chip (empty when not offered)
    std::array<D2D1_RECT_F, 8> commands{};
};

// search_min_dip widens the search field while a search is active or being
// edited, borrowing from the address bar but leaving it at least 120 DIP.
inline ToolbarLayout MakeToolbarLayout(float width, float scale, float top, float margin, float create_width,
                                      float left, float filter_expand = 0.0f, float search_min_dip = 0.0f,
                                      float group_width = 0.0f, float sort_width = 0.0f,
                                      float filter_width = 0.0f) {
    ToolbarLayout out;
    const float available = width - left;
    const float nav_step = NavigationStep(available, scale);
    float x = left + margin;
    for (int i=0;i<3;++i) {
        out.navigation[i] = NavigationButtonAt(x, top + 6 * scale, scale, nav_step);
        x += nav_step;
    }
    float search_width = std::clamp(available*0.27f, 140*scale, 360*scale);
    if (search_min_dip > 0.0f && available >= 480*scale) {
        // While a query is being edited (>= 500 DIP requested) the address bar may
        // shrink to 60 DIP; a results page keeps 120 DIP of breadcrumb.
        const float address_min = (search_min_dip >= 500.0f ? 60.0f : 120.0f) * scale;
        const float cap = width - margin - (x + margin + address_min) - nav_step - 2*margin;
        search_width = std::max(search_width, std::min(search_min_dip*scale, cap));
    }
    out.search = {width-margin-search_width,top+4*scale,width-margin,top+40*scale};
    out.address = {x+margin,top+4*scale,std::max(x+margin+60*scale,out.search.left-margin),top+40*scale};
    if (available < 480*scale) {
        out.search.left=out.search.right-32*scale;
    }
    out.navigation[3] = NavigationButtonAt(out.search.left - margin - nav_step, top + 6 * scale, scale, nav_step);
    out.address.right = out.navigation[3].left-margin;
    const float row_left = left + margin;
    const float row_right = (std::max)(row_left, width - margin);
    const float budget = row_right - row_left;
    float create_w = create_width;
    bool edits = available >= 540 * scale;
    bool views = edits;
    float sort_w = available >= 700*scale ? (sort_width > 0.0f ? sort_width : 88*scale) : 32*scale;
    float filter_base = available >= 700*scale
        ? (filter_width > 0.0f ? (std::max)(filter_width, 64*scale) : 88*scale) : 32*scale;
    float group_w = group_width;
    const auto required = [&] {
        return create_w + (edits ? 12 * scale + 6 * 34 * scale : 0) + 8 * scale + sort_w + 6 * scale +
            filter_base + (group_w > 0 ? 6 * scale + group_w : 0) + 12 * scale + (views ? 3 * 34 * scale + 6 * scale : 0) + 32 * scale;
    };
    // Reserve More first, then compact labels and move commands into it. Never
    // let an expanded filter/group chip push a trailing button outside the row.
    if (required() > budget && group_w > 0) group_w = 32 * scale;
    if (required() > budget) { sort_w = 32 * scale; filter_base = 32 * scale; }
    if (required() > budget) edits = false;
    if (required() > budget) views = false;
    if (required() > budget) create_w = (std::min)(create_w, 32 * scale);
    if (required() > budget) group_w = 0;
    out.create = {row_left,top+50*scale,row_left+create_w,top+82*scale};
    x = out.create.right;
    if (edits) {
        x += 12 * scale;
        for (int i = 0; i < 5; ++i) {
            out.commands[i] = {x, top + 50 * scale, x + 30 * scale, top + 82 * scale};
            x += 34 * scale;
        }
        out.properties = {x, top + 50 * scale, x + 30 * scale, top + 82 * scale};
        x += 34 * scale;
    }
    x += 8 * scale;
    out.sort = {x,top+50*scale,x+sort_w,top+82*scale};
    x = out.sort.right + 6*scale;
    out.overflow = {row_right - 32*scale,top+50*scale,row_right,top+82*scale};
    const float right_group = out.overflow.left - (views ? 3 * 34 * scale + 6 * scale : 0);
    const float group_room = group_w > 0.0f ? group_w + 6*scale : 0.0f;
    const float filter_max = std::clamp(right_group-12*scale-x-group_room,filter_base,(std::max)(filter_base,220*scale));
    const float filter_w = filter_base + (filter_max-filter_base)*std::clamp(filter_expand,0.0f,1.0f);
    out.filter = {x,top+50*scale,x+filter_w,top+82*scale};
    float tail = out.filter.right;
    if (group_w > 0.0f) out.group = {tail+6*scale,top+50*scale,tail+6*scale+group_w,top+82*scale};
    if (views) for (int i=5;i<8;++i) {
        x = right_group+(i-5)*34*scale;
        out.commands[i] = {x,top+50*scale,x+30*scale,top+82*scale};
    }
    return out;
}

// Trailing "x" of the active group chip (removes grouping).
inline D2D1_RECT_F ToolbarGroupClearRect(const D2D1_RECT_F& chip, float scale) {
    return {chip.right-28*scale,chip.top+5*scale,chip.right-6*scale,chip.bottom-5*scale};
}
}
