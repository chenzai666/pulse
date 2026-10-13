#pragma once

#include <windows.h>
#ifdef PULSE_WITH_SELFTEST
#include <functional>
#endif

namespace pulse {
namespace app { class WorkerPool; }
// UI-thread admission and completion. One process-wide pending request
// coalesces repeated hotkeys; Shell work runs on the existing worker pool.
bool QueueListaryGlobalSearch(app::WorkerPool& worker, HWND owner);
bool TakeListaryGlobalSearchResult(HWND owner, WPARAM token, bool& opened);
void CancelListaryGlobalSearch(HWND owner);
#ifdef PULSE_WITH_SELFTEST
// UI-thread-only test seam; captured when a request is admitted.
void SetListarySearchEntryTestLauncher(std::function<bool()> launcher);
bool RunListarySearchEntryTest();
#endif
}
