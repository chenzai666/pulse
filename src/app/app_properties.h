#pragma once
#include <string>
#include <vector>

namespace pulse {
struct AppState;
void ShowItemProperties(AppState& state, const std::vector<std::wstring>& paths);
void RetirePropertiesOwner(AppState& state);
void RefreshPropertiesTheme(AppState& state);
}
