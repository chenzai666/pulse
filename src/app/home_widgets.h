#pragma once
#include "../ui/home_widgets.h"
#include "../ui/fluent_menu.h"
#include <string_view>

namespace pulse { struct AppState; void HandleHomeWidgetClick(AppState& state, const std::wstring& path, bool file); }
namespace pulse::ui { struct HitTestResult; }

namespace pulse {
std::vector<ui::FluentMenuItem> BuildHomeItemMenu(AppState& state,const std::wstring& path,bool file,bool recent);
bool HandleHomeItemMenuCommand(AppState& state,const std::wstring& path,bool file,bool recent,int command);
void ShowHomeItemMenu(AppState& state,const std::wstring& path,bool file,bool recent,POINT point);
bool HandleHomeWidgetContextMenu(AppState& state,const ui::HitTestResult& hit,POINT point);
#ifdef PULSE_WITH_SELFTEST
void SetHomeMenuCallbackForTest(std::function<int(const std::vector<ui::FluentMenuItem>&)> callback);
#endif
}

namespace pulse::app {
struct SidebarModel;
class PlacesCatalog;
inline constexpr std::wstring_view kHomePath = L"pulse:home";
inline bool IsHomePath(std::wstring_view path) { return path == kHomePath; }
ui::HomeWidgets BuildHomeWidgets(const SidebarModel& sidebar, const PlacesCatalog& places,
                                 uint32_t hidden);
}
