#include "../app/status_center_model.h"
#include "../ui/title_chrome_layout.h"
#include "../ui/toolbar_layout.h"

#include <cstdio>
#include <limits>
#include <vector>

using namespace pulse;

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

bool Visible(const D2D1_RECT_F& rect) { return rect.right > rect.left && rect.bottom > rect.top; }
bool Overlap(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return Visible(a) && Visible(b) && a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

void ModelTests() {
    app::StatusCenterModel model;
    ops::OpStatus operation;
    operation.active = true;
    operation.task_id = 1;
    operation.phase = ops::OpPhase::Running;
    operation.percent = 50;
    operation.source_label = L"source.txt";
    Check(model.RecordOperation(operation) && model.ActiveCount() == 1 && model.records()[0].progress == 0.5,
          "operation progress uses percent/100 and a stable task id");
    operation.percent = 70;
    Check(model.RecordOperation(operation) && model.records().size() == 1 && model.records()[0].progress == 0.7,
          "progress updates the existing operation instead of growing history");
    operation.active = false;
    operation.phase = ops::OpPhase::Failed;
    operation.last_error = L"已取消";
    model.RecordOperation(operation);
    ops::FinishedTask finished;
    finished.task_id = 1;
    finished.sources = {L"C:\\source.txt"};
    model.RecordFinishedOperation(finished);
    operation.active = true;
    model.RecordOperation(operation);
    Check(model.records().size() == 1 && model.records()[0].state == app::StatusTaskState::Cancelled && !model.records()[0].cancellable,
          "a generic finished result or stale progress cannot overwrite the terminal cancellation");

    const HWND first = reinterpret_cast<HWND>(UINT_PTR{10});
    const HWND second = reinterpret_cast<HWND>(UINT_PTR{20});
    app::FileConversionEvent event;
    event.generation = 100;
    event.kind = app::FileConversionEventKind::Capabilities;
    Check(!model.RecordConversion(first, event), "capability queries never create task history");
    model.RecordConversionQueued(first, 100, {L"C:\\photo.jpg"}, L"png");
    Check(model.Find({app::StatusTaskSource::FileConversion, 100, first})->state == app::StatusTaskState::Queued,
          "accepted conversion is visible and cancellable before its host starts");
    event.kind = app::FileConversionEventKind::Progress;
    event.progress = 0.25;
    event.inputs = {L"C:\\photo.jpg"};
    event.target = L"png";
    model.RecordConversion(first, event);
    model.RecordConversion(second, event);
    Check(model.ActiveCount() == 2 && model.Find({app::StatusTaskSource::FileConversion, 100, first})->progress == 0.25,
          "conversion identity includes its window and generation without merging two windows");
    model.MarkCancelling({app::StatusTaskSource::FileConversion, 100, first});
    model.RecordConversion(first, event);
    Check(model.Find({app::StatusTaskSource::FileConversion, 100, first})->state == app::StatusTaskState::Cancelling &&
              !model.Find({app::StatusTaskSource::FileConversion, 100, first})->cancellable,
          "late progress cannot restore cancellation after the user has requested it");
    model.RetireConversionOwner(first);
    Check(model.Find({app::StatusTaskSource::FileConversion, 100, first})->state == app::StatusTaskState::Cancelled &&
              model.Find({app::StatusTaskSource::FileConversion, 100, second})->cancellable,
          "closing one window retires only that window's conversion");
    event.kind = app::FileConversionEventKind::Completed;
    event.state = L"completedwitherrors";
    app::FileConversionFileResult file;
    file.source = L"C:\\photo.jpg";
    file.state = L"failed";
    file.error = L"engine failed";
    event.files.push_back(file);
    model.RecordConversion(second, event);
    const auto* partial = model.Find({app::StatusTaskSource::FileConversion, 100, second});
    Check(partial && partial->state == app::StatusTaskState::CompletedWithErrors && partial->detail == L"engine failed" && !partial->cancellable,
          "partial conversion failure retains an error and disables cancellation");

    operation.task_id = 2;
    operation.active = true;
    operation.type = ops::OpType::EmptyRecycle;
    operation.phase = ops::OpPhase::Running;
    operation.percent = std::numeric_limits<float>::quiet_NaN();
    operation.last_error.clear();
    model.RecordOperation(operation);
    const auto* emptying = model.Find({app::StatusTaskSource::Operation, 2, nullptr});
    Check(emptying && !emptying->cancellable && emptying->progress < 0,
          "non-cancellable recycle engine and non-finite progress are represented honestly");
    for (uint64_t id = 3; id < 100; ++id) {
        finished.task_id = id;
        finished.type = ops::OpType::Copy;
        finished.success = id % 2 == 0;
        model.RecordFinishedOperation(finished);
    }
    finished.task_id = 100;
    finished.success = false;
    finished.last_error = L"Canceled";
    model.RecordFinishedOperation(finished);
    Check(model.Find({app::StatusTaskSource::Operation, 100, nullptr})->state == app::StatusTaskState::Cancelled &&
              model.Find({app::StatusTaskSource::Operation, 100, nullptr})->detail == L"Canceled",
          "finished queue retains cancellation reasons even when the status snapshot was overwritten");
    Check(model.HistoryCount() == app::StatusCenterModel::kMaxHistory && model.ActiveCount() == 1,
          "quick serial completions are recorded and capped without evicting an active operation");
    model.ClearCompleted();
    model.RecordFinishedOperation(finished);
    Check(model.ActiveCount() == 1 && model.HistoryCount() == 0,
          "clearing finished history keeps active work and rejects already-cleared late notifications");
    operation.active = false;
    operation.phase = ops::OpPhase::Completed;
    model.RecordOperation(operation);
    Check(model.HistoryCount() == 1 && model.ActiveCount() == 0,
          "an older active task still completes after clearing newer history");
}

void LayoutTests() {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (float dip_width : {480.0f, 640.0f, 680.0f, 900.0f, 1280.0f}) {
            const float width = dip_width * scale;
            const auto chrome = ui::MakeTitleChrome(width, scale, 48 * scale);
            Check(chrome.status_w == 0 || (chrome.group_left <= chrome.status_left &&
                      chrome.status_left + chrome.status_w < chrome.settings_left && chrome.group_right < chrome.chrome_left),
                  "status button shares title geometry without overlapping settings or window controls");
            Check((chrome.status_w > 0) == (dip_width >= 680), "narrow title bars move the status entry into More");
            for (float sidebar : {52.0f, 224.0f}) {
                const auto budget_chrome = ui::MakeTitleChrome(width, scale, 48 * scale, sidebar * scale);
                Check((budget_chrome.status_w > 0) == (dip_width >= 680 && dip_width - sidebar >= 600),
                      "expanded sidebars also reserve title/address space before showing the status icon");
                for (float expand : {0.0f, 1.0f}) {
                    const float left = sidebar * scale;
                    const auto layout = ui::MakeToolbarLayout(width, scale, 48 * scale, 12 * scale, 86 * scale,
                        left, expand, 0, 212 * scale, 104 * scale, 84 * scale);
                    std::vector<D2D1_RECT_F> controls{layout.create, layout.sort, layout.filter, layout.group,
                        layout.properties, layout.overflow};
                    controls.insert(controls.end(), layout.commands.begin(), layout.commands.end());
                    bool fits = Visible(layout.overflow);
                    for (size_t i = 0; i < controls.size(); ++i) {
                        if (!Visible(controls[i])) continue;
                        fits = fits && controls[i].left >= left && controls[i].right <= width;
                        for (size_t j = i + 1; j < controls.size(); ++j) fits = fits && !Overlap(controls[i], controls[j]);
                    }
                    Check(fits, "command row stays inside narrow/DPI bounds with no overlapping hit targets");
                    if (dip_width == 1280) Check(Visible(layout.properties), "wide command row exposes a Properties button after edit icons");
                    if (dip_width == 480) Check(!Visible(layout.properties) && Visible(layout.overflow), "narrow Properties remains available through More");
                }
            }
        }
    }
}

} // namespace

int main() {
    ModelTests();
    LayoutTests();
    return failures ? 1 : 0;
}
