#pragma once

#include "status_center_model.h"

namespace pulse {
struct AppState;
void ShowStatusCenter(AppState& state);
void RecordOperationStatus(AppState& state, const ops::OpStatus& status);
// Pass each item from the existing drain; this function never drains Ops.
void RecordFinishedOperation(AppState& state, const ops::FinishedTask& task);
void RecordFileConversionQueued(AppState& state, uint64_t generation, const std::vector<std::wstring>& inputs,
                                const std::wstring& target);
// Call before retiring fileConversionUi.job_generation in the event handler.
void RecordFileConversionStatus(AppState& state, const app::FileConversionEvent& event);
void RetireStatusCenterOwner(AppState& state);
void ShowToolbarProperties(AppState& state);
} // namespace pulse
