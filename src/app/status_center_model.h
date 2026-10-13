#pragma once

#include "file_conversion_integration.h"
#include "../ops/ops_manager.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::app {

enum class StatusTaskSource { Operation, FileConversion };
enum class StatusTaskState { Queued, Running, Paused, Waiting, Cancelling,
                             Completed, CompletedWithErrors, Failed, Cancelled };

struct StatusTaskKey {
    StatusTaskSource source = StatusTaskSource::Operation;
    uint64_t id = 0;
    HWND owner = nullptr;
    bool operator==(const StatusTaskKey&) const = default;
};

struct StatusTaskRecord {
    StatusTaskKey key;
    ops::OpType operation = ops::OpType::Copy;
    StatusTaskState state = StatusTaskState::Queued;
    double progress = -1.0; // fraction; negative means unknown
    bool cancellable = false;
    std::wstring source;
    std::wstring destination;
    std::wstring target;
    std::wstring detail;
    uint64_t item_count = 0;
    bool operator==(const StatusTaskRecord&) const = default;
};

bool StatusTaskTerminal(StatusTaskState state) noexcept;

// UI-thread model shared by AppCore. Completed history is bounded; active
// work is retained until its terminal event, without polling the engines.
class StatusCenterModel {
public:
    static constexpr size_t kMaxHistory = 64;
    bool RecordOperation(const ops::OpStatus& status);
    bool RecordFinishedOperation(const ops::FinishedTask& task);
    bool RecordConversionQueued(HWND owner, uint64_t generation, const std::vector<std::wstring>& inputs,
                                const std::wstring& target);
    // Call only for a conversion job, never for capability-query errors.
    bool RecordConversion(HWND owner, const FileConversionEvent& event);
    bool RetireConversionOwner(HWND owner);
    bool MarkCancelling(const StatusTaskKey& key);
    bool ClearCompleted();
    size_t ActiveCount() const noexcept;
    size_t HistoryCount() const noexcept;
    const std::vector<StatusTaskRecord>& records() const noexcept { return records_; }
    const StatusTaskRecord* Find(const StatusTaskKey& key) const noexcept;

private:
    bool Store(StatusTaskRecord record);
    void Prune();
    std::vector<StatusTaskRecord> records_;
    uint64_t cleared_operation_id_ = 0;
    uint64_t cleared_conversion_id_ = 0;
};

} // namespace pulse::app
