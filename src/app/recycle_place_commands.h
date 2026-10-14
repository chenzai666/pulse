#pragma once
#include <vector>
#include <string>
#include "../fs/fs_enum.h"

namespace pulse {
struct AppState;
bool CanOpenRecycleInNewPane(const AppState& state);
bool HandleRecyclePlaceCommand(AppState& state, int command);
bool HandleRecycleToolbarCommand(AppState& state, int action);
std::vector<std::wstring> RecycleRestorePaths(const std::vector<fs::DirEntry>& entries);
}
