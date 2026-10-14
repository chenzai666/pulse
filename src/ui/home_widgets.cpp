#include "home_widgets.h"
#include "fluent_components.h"
#include "ui_renderer.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
struct Slot { D2D1_RECT_F bounds{}; int section=-1,item=-1; };
struct Layout { std::vector<Slot> slots; float height=0; };
Layout MakeLayout(const HomeWidgets& widgets,float width,float scale) {
    Layout layout;
    const float margin=16*scale, gap=8*scale;
    const float inner=std::max(1.0f,width-2*margin);
    float y=16*scale;
    for(size_t group=0;group<widgets.sections.size();++group) {
        const auto& section=widgets.sections[group];
        if(!section.visible) continue;
        const bool quick=group==0;
        const int columns=std::max(1,std::min(quick ? 6 : 4,static_cast<int>((inner+gap)/((quick ? 168 : 240)*scale+gap))));
        const float cell=(inner-gap*(columns-1))/columns;
        const float card_height=(quick ? 88 : 80)*scale;
        const float row_height=card_height+gap;
        layout.slots.push_back({D2D1::RectF(margin,y,width-margin,y+30*scale),static_cast<int>(group),-1});
        y+=36*scale;
        if(section.items.empty()) {
            layout.slots.push_back({D2D1::RectF(margin,y,width-margin,y+48*scale),static_cast<int>(group),-2});
            y+=48*scale;
        } else {
            for(size_t item=0;item<section.items.size();++item) {
                const float x=margin+static_cast<float>(item%static_cast<size_t>(columns))*(cell+gap);
                const float top=y+static_cast<float>(item/static_cast<size_t>(columns))*row_height;
                layout.slots.push_back({D2D1::RectF(x,top,x+cell,top+card_height),static_cast<int>(group),static_cast<int>(item)});
            }
            y+=static_cast<float>((section.items.size()+columns-1)/columns)*row_height;
        }
        y+=16*scale;
    }
    layout.height=y+16*scale;
    return layout;
}
D2D1_RECT_F Offset(D2D1_RECT_F r,const D2D1_RECT_F& bounds,float scroll) {
    r.left+=bounds.left;r.right+=bounds.left;r.top+=bounds.top-scroll;r.bottom+=bounds.top-scroll;return r;
}
bool Contains(const D2D1_RECT_F& r,float x,float y) {return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}
}
float HomeWidgetsHeight(const HomeWidgets& widgets,float width,float scale) {
    return MakeLayout(widgets,width,scale).height;
}
HomeWidgetHit HitHomeWidgets(const HomeWidgets& widgets,const D2D1_RECT_F& bounds,
                            float scroll,float scale,float x,float y) {
    if(!Contains(bounds,x,y)) return {};
    const auto layout=MakeLayout(widgets,bounds.right-bounds.left,scale);
    for(const auto& slot:layout.slots) {
        if(slot.item<0 || !Contains(Offset(slot.bounds,bounds,scroll),x,y)) continue;
        const auto& item=widgets.sections[slot.section].items[slot.item];
        return {slot.section*100000+slot.item,item.path,item.file};
    }
    return {};
}
void DrawHomeWidgets(fluent::Painter& painter,Compositor& compositor,const Theme& theme,
                     const HomeWidgets& widgets,const D2D1_RECT_F& bounds,
                     float scroll,float scale,int hovered,
                     const std::function<bool(std::wstring_view,const D2D1_RECT_F&)>& draw_icon) {
    auto* dc=compositor.Dc();if(!dc)return;
    dc->PushAxisAlignedClip(bounds,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const auto layout=MakeLayout(widgets,bounds.right-bounds.left,scale);
    bool visible=false;
    for(const auto& slot:layout.slots) {
        visible=true;
        const auto r=Offset(slot.bounds,bounds,scroll);
        if(r.bottom<bounds.top || r.top>bounds.bottom)continue;
        const auto& group=widgets.sections[slot.section];
        if(slot.item<0) {
            painter.DrawText(slot.item==-1 ? group.title : group.empty_text,r,
                slot.item==-1 ? compositor.TextFormat() : compositor.SmallFormat(),
                slot.item==-1 ? theme.text : theme.text_secondary);
            continue;
        }
        const auto& item=group.items[slot.item];
        const bool hover=hovered==slot.section*100000+slot.item;
        painter.FillRoundedRect(r,8*scale,hover ? theme.fill_hover : theme.surface_card);
        painter.StrokeRoundedRect(r,8*scale,theme.stroke_card);
        const auto color=item.color ? HexColor(item.color) : item.icon_color.a>0 ? item.icon_color : theme.accent;
        const bool quick=slot.section==0;
        const float center=(r.left+r.right)*0.5f;
        const auto icon=quick ? D2D1::RectF(center-16*scale,r.top+12*scale,center+16*scale,r.top+44*scale)
            : D2D1::RectF(r.left+12*scale,r.top+24*scale,r.left+40*scale,r.top+52*scale);
        if(item.color || !draw_icon || !draw_icon(item.glyph,icon)) painter.DrawGlyph(item.glyph,icon,color);
        if(quick) {
            painter.DrawText(item.title,D2D1::RectF(r.left+8*scale,r.top+49*scale,r.right-8*scale,r.bottom-8*scale),
                compositor.TextFormat(),theme.text,fluent::HorizontalAlignment::Center);
            // 图钉表示固定入口；卡片整体仍只执行导航。
            if(item.pinned) painter.DrawGlyph(L"\xE718",D2D1::RectF(r.right-22*scale,r.top+4*scale,r.right-4*scale,r.top+22*scale),theme.text_secondary);
        } else {
            painter.DrawText(item.title,D2D1::RectF(r.left+50*scale,r.top+7*scale,r.right-12*scale,r.top+33*scale),compositor.TextFormat(),theme.text);
            painter.DrawText(item.detail,D2D1::RectF(r.left+50*scale,r.top+(item.drive ? 46 : 34)*scale,r.right-12*scale,r.bottom-6*scale),compositor.SmallFormat(),theme.text_secondary);
        }
        if(item.drive) {
            const auto track=D2D1::RectF(r.left+50*scale,r.top+37*scale,r.right-12*scale,r.top+40*scale);
            painter.FillRoundedRect(track,1.5f*scale,theme.stroke_card);
            auto fill=track;fill.right=fill.left+(fill.right-fill.left)*std::clamp(item.used_ratio,0.0f,1.0f);
            if(fill.right>fill.left)painter.FillRoundedRect(fill,1.5f*scale,item.danger ? HexColor(0xEF4444) : theme.accent);
        }
    }
    if(!visible) painter.DrawText(l10n::Pick(L"主页小组件已全部关闭，可在设置中重新启用。",L"All home widgets are hidden. Enable them in Settings."),
        Offset(D2D1::RectF(16*scale,70*scale,bounds.right-bounds.left-16*scale,126*scale),bounds,scroll),compositor.TextFormat(),theme.text_secondary);
    dc->PopAxisAlignedClip();
}
}
