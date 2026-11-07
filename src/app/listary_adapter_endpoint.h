#pragma once

#include <windows.h>

namespace pulse {
struct AppState;

bool AttachListaryAdapter(AppState& state);
void DetachListaryAdapter(AppState& state) noexcept;
bool ListaryAdapterMessage(AppState& state, UINT message, WPARAM wparam,
                           LPARAM lparam, LRESULT& result);
} // namespace pulse
