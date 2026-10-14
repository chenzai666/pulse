#pragma once
#include "../ui/home_widgets.h"
#include <string_view>

namespace pulse { struct AppState; void HandleHomeWidgetClick(AppState& state, const std::wstring& path, bool file); }

namespace pulse::app {
struct SidebarModel;
class PlacesCatalog;
inline constexpr std::wstring_view kHomePath = L"pulse:home";
inline bool IsHomePath(std::wstring_view path) { return path == kHomePath; }
ui::HomeWidgets BuildHomeWidgets(const SidebarModel& sidebar, const PlacesCatalog& places,
                                 uint32_t hidden);
}
