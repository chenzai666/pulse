#include "../ops/operation_presentation.h"
#include <cstdio>
#include <limits>

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    using pulse::path::FriendlyPathText;
    check(FriendlyPathText(L"删除 \\\\?\\C:\\文件 完成") == L"删除 C:\\文件 完成", "embedded extended drive path");
    check(FriendlyPathText(L"\\??\\C:\\文件") == L"C:\\文件", "NT drive path");
    check(FriendlyPathText(L"\\\\?\\UNC\\server\\share\\文件") == L"\\\\server\\share\\文件", "extended UNC path");
    check(FriendlyPathText(L"失败：\\??\\unc\\server\\share") == L"失败：\\\\server\\share", "embedded NT UNC path");
    check(FriendlyPathText(L"C:\\文件 → \\\\server\\share") == L"C:\\文件 → \\\\server\\share", "ordinary paths unchanged");
    check(FriendlyPathText(L"\\\\?\\Volume{abc}\\") == L"\\\\?\\Volume{abc}\\", "unknown device path preserved");
    pulse::ops::OpStatus status;
    status.type = pulse::ops::OpType::RecycleDelete;
    status.phase = pulse::ops::OpPhase::Completed;
    status.percent = -1.0f;
    status.summary = L"删除 \\\\?\\C:\\文件 完成";
    status.last_error = L"无法访问 \\??\\C:\\文件";
    status.source_label = status.destination_label = status.current_item = L"\\\\?\\C:\\文件";
    const auto shown = pulse::ops::PresentOperationStatus(status);
    check(shown.percent == 100.0f, "completed shell deletion has full progress");
    check(shown.summary == L"删除 C:\\文件 完成" && shown.last_error == L"无法访问 C:\\文件" &&
          shown.source_label == L"C:\\文件" && shown.destination_label == L"C:\\文件" &&
          shown.current_item == L"C:\\文件", "all operation display fields sanitized");
    check(status.percent == -1.0f && status.current_item == L"\\\\?\\C:\\文件", "original status unchanged");
    for (auto phase : {pulse::ops::OpPhase::Running, pulse::ops::OpPhase::Paused,
                       pulse::ops::OpPhase::Failed, pulse::ops::OpPhase::Cancelling}) {
        status.phase = phase;
        status.percent = 37.0f;
        check(pulse::ops::PresentOperationStatus(status).percent == 37.0f, "unfinished progress preserved");
    }
    status.type = pulse::ops::OpType::EmptyRecycle;
    status.active = true;
    status.phase = pulse::ops::OpPhase::Running;
    status.percent = 35;
    status.total_items = 60622;
    status.total_bytes = 14400000000ull;
    status.bytes_per_second = 1234;
    status.speed_basis = pulse::ops::OpSpeedBasis::Bytes;
    const auto empty = pulse::ops::PresentOperationStatus(status);
    check(empty.percent < 0 && empty.total_items == 0 && empty.total_bytes == 0 &&
          empty.speed_basis == pulse::ops::OpSpeedBasis::None && empty.bytes_per_second == 0,
          "empty recycle never presents estimated polling data as measured progress");
    check(status.total_items == 60622 && status.percent == 35, "presentation keeps engine state intact");
    status.type = pulse::ops::OpType::RealDelete;
    status.percent = -1;
    status.completed_items = status.transferred_bytes = 0;
    status.bytes_per_second = status.items_per_second = 0;
    status.peak_bytes_per_second = status.peak_items_per_second = 0;
    check(pulse::ops::PresentOperationStatus(status).speed_basis == pulse::ops::OpSpeedBasis::None,
          "unknown delete without measurements has no fabricated speed graph");
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), -10.0f}) {
        status.percent = invalid;
        check(pulse::ops::PresentOperationStatus(status).percent == -1,
              "invalid progress becomes indeterminate");
    }
    status.type = pulse::ops::OpType::Copy;
    status.percent = 150;
    check(pulse::ops::PresentOperationStatus(status).percent == 100, "progress is bounded");
    status.percent = 42;
    status.speed_basis = pulse::ops::OpSpeedBasis::Bytes;
    status.transferred_bytes = 512;
    status.bytes_per_second = 128;
    const auto copy = pulse::ops::PresentOperationStatus(status);
    check(copy.percent == 42 && copy.bytes_per_second == 128 &&
          copy.speed_basis == pulse::ops::OpSpeedBasis::Bytes, "measured transfer progress remains available");
    check(pulse::ops::SpeedBasisFor(pulse::ops::OpType::EmptyRecycle) == pulse::ops::OpSpeedBasis::None,
          "empty recycle has no measured speed basis");
    return failures ? 1 : 0;
}
