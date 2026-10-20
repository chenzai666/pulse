// startup_location.cpp — see startup_location.h.
#include "startup_location.h"

#include "app_prefs.h"

namespace pulse::app {

std::wstring DefaultLocation(const AppPrefs& prefs) {
    return prefs.home_folder;
}

bool RestoresLastTabs(const AppPrefs& prefs, bool restore_update_session) noexcept {
    return restore_update_session || prefs.startup_open != 1;
}

std::wstring NewTabLocation(const AppPrefs& prefs, const std::wstring& current) {
    if (prefs.new_tab_open == 2) return L"pulse:home";
    return prefs.new_tab_open == 1 ? DefaultLocation(prefs) : current;
}

} // namespace pulse::app
