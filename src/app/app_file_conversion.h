#pragma once
#include "file_conversion_integration.h"
#include "../ui/fluent_menu.h"
#include <optional>

namespace pulse {
struct AppState;

struct FileConversionUiState {
    uint64_t capabilities_generation = 0;
    uint64_t job_generation = 0;
    POINT menu_point{};
    bool choosing = false;
    bool selecting = false;
    std::optional<app::FileConversionEvent> pending_choice;
    ui::FluentMenu* active_menu = nullptr; // borrowed only while its stack-owned popup is active
    ULONGLONG last_progress_tick = 0;
    int progress_percent = 0;
    size_t input_count = 0;
    std::wstring target_label;
    std::vector<std::wstring> completed_outputs;
};

void AppendFileConversionCommand(AppState& state, std::vector<ui::FluentMenuItem>& items);
bool HandleFileConversionCommand(AppState& state, int command,
                                 const std::vector<std::wstring>& inputs, POINT menu_point);
bool HandleFileConversionMessage(AppState& state, UINT message, WPARAM wparam, LPARAM token);
void ShutdownFileConversion(AppState& state);
}
