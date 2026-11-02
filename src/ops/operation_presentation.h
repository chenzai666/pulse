#pragma once

#include "ops_manager.h"
#include "../common/display_path.h"
#include <algorithm>
#include <cmath>

namespace pulse::ops {

inline OpStatus PresentOperationStatus(OpStatus status) {
    if (status.phase == OpPhase::Completed) status.percent = 100.0f;
    else if (!std::isfinite(status.percent) || status.percent < 0) status.percent = -1.0f;
    else status.percent = std::clamp(status.percent, 0.0f, 100.0f);
    if (status.type == OpType::EmptyRecycle) {
        status.speed_basis = OpSpeedBasis::None;
        status.bytes_per_second = status.peak_bytes_per_second = 0;
        status.items_per_second = status.peak_items_per_second = 0;
        status.eta_seconds = 0;
        if (status.active && status.phase != OpPhase::Completed) {
            status.percent = -1;
            status.total_items = status.completed_items = 0;
            status.total_bytes = status.transferred_bytes = 0;
        }
    } else if (status.active && status.percent < 0 && status.completed_items == 0 &&
               status.transferred_bytes == 0 && status.bytes_per_second == 0 &&
               status.items_per_second == 0 && status.peak_bytes_per_second == 0 &&
               status.peak_items_per_second == 0) {
        status.speed_basis = OpSpeedBasis::None;
    }
    status.summary = path::FriendlyPathText(status.summary);
    status.last_error = path::FriendlyPathText(status.last_error);
    status.source_label = path::FriendlyPathText(status.source_label);
    status.destination_label = path::FriendlyPathText(status.destination_label);
    status.current_item = path::FriendlyPathText(status.current_item);
    return status;
}

} // namespace pulse::ops
