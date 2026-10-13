#pragma once
#include <windows.h>

namespace pulse {
struct AppState;
void HandleEmbeddedQuickLookEvent(AppState& state, UINT_PTR token);
}
