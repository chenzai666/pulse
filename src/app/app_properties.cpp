#include "app_properties.h"
#include "app_internal.h"
#include "../ui/properties_window.h"

namespace pulse {

void ShowItemProperties(AppState& state, const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    auto& core = state.core;
    if (!core.propertiesWindow) core.propertiesWindow = std::make_unique<ui::PropertiesWindow>();
    if (!core.propertiesWindow->Create(state.hwnd, [&core](const std::vector<std::wstring>& selected) {
        core.ops.ShowProperties(selected);
    })) return;
    core.propertiesWindow->SetTheme(state.darkMode, state.accentColor);
    core.propertiesWindow->Show(paths);
}

void RetirePropertiesOwner(AppState& state) {
    if (state.core.propertiesWindow && state.core.propertiesWindow->owner() == state.hwnd)
        state.core.propertiesWindow->Destroy();
}

void RefreshPropertiesTheme(AppState& state) {
    if (state.core.propertiesWindow)
        state.core.propertiesWindow->SetTheme(state.darkMode, state.accentColor);
}

}
