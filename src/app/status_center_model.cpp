#include "status_center_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pulse::app {
namespace {

std::wstring Bounded(std::wstring value) {
    constexpr size_t maximum = 2048;
    if (value.size() > maximum) {
        value.resize(maximum);
        if (value.back() >= 0xD800 && value.back() <= 0xDBFF) value.pop_back();
    }
    return value;
}

double Fraction(double progress) {
    return !std::isfinite(progress) || progress < 0 ? -1.0 : std::clamp(progress, 0.0, 1.0);
}

bool CancelledError(const std::wstring& error) {
    return error == L"已取消" || error == L"Canceled" || error == L"Cancelled";
}

} // namespace

bool StatusTaskTerminal(StatusTaskState state) noexcept {
    return state == StatusTaskState::Completed || state == StatusTaskState::CompletedWithErrors ||
        state == StatusTaskState::Failed || state == StatusTaskState::Cancelled;
}

const StatusTaskRecord* StatusCenterModel::Find(const StatusTaskKey& key) const noexcept {
    const auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& record) { return record.key == key; });
    return found == records_.end() ? nullptr : &*found;
}

bool StatusCenterModel::Store(StatusTaskRecord record) {
    if (!record.key.id) return false;
    auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& existing) { return existing.key == record.key; });
    if (found != records_.end()) {
        // Terminal records cannot be revived by a late progress notification.
        if (StatusTaskTerminal(found->state) || *found == record) return false;
        if (found->state == StatusTaskState::Cancelling && !StatusTaskTerminal(record.state)) {
            record.state = StatusTaskState::Cancelling;
            record.cancellable = false;
        }
        if (*found == record) return false;
        *found = std::move(record);
    } else {
        const uint64_t cleared = record.key.source == StatusTaskSource::Operation
            ? cleared_operation_id_ : cleared_conversion_id_;
        if (record.key.id <= cleared) return false;
        records_.insert(records_.begin(), std::move(record));
    }
    Prune();
    return true;
}

void StatusCenterModel::Prune() {
    std::stable_partition(records_.begin(), records_.end(), [](const auto& record) { return !StatusTaskTerminal(record.state); });
    size_t history = 0;
    for (auto it = records_.begin(); it != records_.end();) {
        if (StatusTaskTerminal(it->state) && ++history > kMaxHistory) it = records_.erase(it);
        else ++it;
    }
}

bool StatusCenterModel::RecordOperation(const ops::OpStatus& status) {
    if (!status.task_id) return false;
    StatusTaskRecord record;
    record.key = {StatusTaskSource::Operation, status.task_id, nullptr};
    record.operation = status.type;
    record.source = Bounded(status.source_label);
    record.destination = Bounded(status.destination_label);
    record.item_count = status.total_items;
    record.progress = Fraction(static_cast<double>(status.percent) / 100.0);
    record.detail = Bounded(!status.last_error.empty() ? status.last_error : status.current_item);
    if (!status.active) {
        record.state = status.phase == ops::OpPhase::Completed ? StatusTaskState::Completed
            : CancelledError(status.last_error) ? StatusTaskState::Cancelled : StatusTaskState::Failed;
        if (record.state == StatusTaskState::Completed) record.progress = 1.0;
    } else {
        switch (status.phase) {
        case ops::OpPhase::Queued: record.state = StatusTaskState::Queued; break;
        case ops::OpPhase::Paused: record.state = StatusTaskState::Paused; break;
        case ops::OpPhase::WaitingForConflict: record.state = StatusTaskState::Waiting; break;
        case ops::OpPhase::Cancelling: record.state = StatusTaskState::Cancelling; break;
        default: record.state = StatusTaskState::Running; break;
        }
        if (status.authorization != ops::AuthorizationState::None) record.state = StatusTaskState::Waiting;
        // SHEmptyRecycleBin has no cancellable request in the current engine.
        record.cancellable = status.type != ops::OpType::EmptyRecycle && record.state != StatusTaskState::Cancelling;
    }
    return Store(std::move(record));
}

bool StatusCenterModel::RecordFinishedOperation(const ops::FinishedTask& task) {
    if (!task.task_id) return false;
    const StatusTaskKey key{StatusTaskSource::Operation, task.task_id, nullptr};
    StatusTaskRecord record;
    if (const auto* previous = Find(key)) record = *previous;
    record.key = key;
    record.operation = task.type;
    record.state = task.success ? StatusTaskState::Completed
        : CancelledError(task.last_error) ? StatusTaskState::Cancelled : StatusTaskState::Failed;
    record.cancellable = false;
    record.progress = task.success ? 1.0 : record.progress;
    record.item_count = task.sources.size();
    if (!task.sources.empty()) record.source = Bounded(task.sources.front());
    if (!task.last_error.empty()) record.detail = Bounded(task.last_error);
    return Store(std::move(record));
}

bool StatusCenterModel::RecordConversionQueued(HWND owner, uint64_t generation,
                                              const std::vector<std::wstring>& inputs, const std::wstring& target) {
    if (!owner || !generation) return false;
    StatusTaskRecord record;
    record.key = {StatusTaskSource::FileConversion, generation, owner};
    record.cancellable = true;
    record.item_count = inputs.size();
    if (!inputs.empty()) record.source = Bounded(inputs.front());
    record.target = Bounded(target);
    return Store(std::move(record));
}

bool StatusCenterModel::RecordConversion(HWND owner, const FileConversionEvent& event) {
    if (!owner || !event.generation || event.kind == FileConversionEventKind::Capabilities) return false;
    StatusTaskRecord record;
    record.key = {StatusTaskSource::FileConversion, event.generation, owner};
    if (const auto* previous = Find(record.key)) record = *previous;
    record.item_count = event.inputs.empty() ? event.file_count : event.inputs.size();
    if (!event.inputs.empty()) record.source = Bounded(event.inputs.front());
    if (!event.target.empty()) record.target = Bounded(event.target);
    record.detail = Bounded(event.error_text);
    record.cancellable = true;
    switch (event.kind) {
    case FileConversionEventKind::Started: record.state = StatusTaskState::Running; break;
    case FileConversionEventKind::Progress:
        record.state = StatusTaskState::Running;
        record.progress = Fraction(event.progress);
        break;
    case FileConversionEventKind::Completed:
        record.state = event.state == L"cancelled" ? StatusTaskState::Cancelled
            : event.state == L"failed" ? StatusTaskState::Failed
            : event.state == L"completedwitherrors" ? StatusTaskState::CompletedWithErrors : StatusTaskState::Completed;
        record.progress = record.state == StatusTaskState::Completed ? 1.0 : record.progress;
        record.cancellable = false;
        for (const auto& file : event.files) {
            if (record.destination.empty() && !file.outputs.empty()) record.destination = Bounded(file.outputs.front());
            if (record.detail.empty() && !file.error.empty()) record.detail = Bounded(file.error);
        }
        break;
    case FileConversionEventKind::Failed:
        record.state = event.error == ERROR_OPERATION_ABORTED || event.error_key == L"cancelled"
            ? StatusTaskState::Cancelled : StatusTaskState::Failed;
        record.cancellable = false;
        break;
    case FileConversionEventKind::Stopped:
        record.state = StatusTaskState::Cancelled;
        record.cancellable = false;
        break;
    default: return false;
    }
    return Store(std::move(record));
}

bool StatusCenterModel::RetireConversionOwner(HWND owner) {
    bool changed = false;
    for (auto& record : records_) {
        if (record.key.source == StatusTaskSource::FileConversion && record.key.owner == owner && !StatusTaskTerminal(record.state)) {
            record.state = StatusTaskState::Cancelled;
            record.cancellable = false;
            changed = true;
        }
    }
    if (changed) Prune();
    return changed;
}

bool StatusCenterModel::MarkCancelling(const StatusTaskKey& key) {
    auto found = std::find_if(records_.begin(), records_.end(), [&](const auto& record) { return record.key == key; });
    if (found == records_.end() || !found->cancellable || StatusTaskTerminal(found->state)) return false;
    found->state = StatusTaskState::Cancelling;
    found->cancellable = false;
    return true;
}

bool StatusCenterModel::ClearCompleted() {
    const size_t before = records_.size();
    std::erase_if(records_, [&](const auto& record) {
        if (!StatusTaskTerminal(record.state)) return false;
        auto& watermark = record.key.source == StatusTaskSource::Operation ? cleared_operation_id_ : cleared_conversion_id_;
        watermark = (std::max)(watermark, record.key.id);
        return true;
    });
    return before != records_.size();
}

size_t StatusCenterModel::ActiveCount() const noexcept {
    return static_cast<size_t>(std::count_if(records_.begin(), records_.end(), [](const auto& record) { return !StatusTaskTerminal(record.state); }));
}

size_t StatusCenterModel::HistoryCount() const noexcept { return records_.size() - ActiveCount(); }

} // namespace pulse::app
