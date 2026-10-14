// startup_location.h — Where Pulse opens at startup and in new tabs
// (#31, #34, #41, #44). An empty location means This PC.
#pragma once

#include <string>

namespace pulse::app {

struct AppPrefs;

// 默认位置：pulse:home、文件夹路径，或空字符串表示此电脑。
std::wstring DefaultLocation(const AppPrefs& prefs);

// True when startup should bring back the previous session's tabs.
bool RestoresLastTabs(const AppPrefs& prefs, bool restore_update_session = false) noexcept;

// Where a tab the user creates opens. `current` is the folder the
// current-folder rule picked; it is kept unless the default location is chosen.
std::wstring NewTabLocation(const AppPrefs& prefs, const std::wstring& current);

} // namespace pulse::app
