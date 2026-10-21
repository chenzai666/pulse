#include "../app/explorer_takeover_guard.h"
#include "../app/shell_window_plan.h"

#include <cstdio>

namespace {

using pulse::app::ExplorerTakeoverGuard;
using pulse::app::ExplorerWindowIdentity;

int failures = 0;

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    if (!passed) ++failures;
}

void TimedOutWindowReceivesLateEvents() {
    ExplorerTakeoverGuard guard;
    const ExplorerWindowIdentity window{0x100, 10, 11, 1000};
    const auto foreground_token = guard.Track(window);
    Check(foreground_token && guard.IsCurrent(window, foreground_token),
        "foreground event tracks a current Explorer generation");
    Check(guard.AllowReassert(window, foreground_token),
        "initial style reset may reassert concealment before timeout");
    Check(guard.Release(window, foreground_token),
        "view timeout permanently releases the current Explorer generation");

    const auto show_token = guard.Track(window);
    Check(show_token == foreground_token && guard.IsReleased(window, show_token) &&
        !guard.AllowReassert(window, show_token),
        "SHOW emitted during restoration cannot conceal the released window again");

    const auto registered_token = guard.Track(window);
    Check(registered_token == foreground_token && guard.IsReleased(window, registered_token) &&
        !guard.AllowReassert(window, registered_token),
        "late shell registration preserves the timeout release and skips another takeover");
}

void ReusedWindowRejectsOldCallbacks() {
    const ExplorerWindowIdentity original{0x200, 20, 21, 2000};
    const ExplorerWindowIdentity replacements[] = {
        {original.window, 22, original.thread_id, original.process_created},
        {original.window, original.process_id, 23, original.process_created},
        {original.window, original.process_id, original.thread_id, 2001},
    };
    const char* labels[] = {
        "reused HWND with changed PID rejects the previous generation",
        "reused HWND with changed thread rejects the previous generation",
        "reused HWND with changed process creation time rejects the previous generation",
    };
    for (unsigned index = 0; index < 3; ++index) {
        ExplorerTakeoverGuard guard;
        const auto old_token = guard.Track(original);
        for (unsigned attempt = 0; attempt < ExplorerTakeoverGuard::kMaxReassertions; ++attempt)
            guard.AllowReassert(original, old_token);
        guard.Release(original, old_token);

        const auto& replacement = replacements[index];
        const auto new_token = guard.Track(replacement);
        Check(new_token && new_token != old_token && guard.IsCurrent(replacement, new_token) &&
            !guard.IsCurrent(original, old_token) && !guard.IsCurrent(replacement, old_token) &&
            !guard.IsReleased(replacement, new_token) && guard.AllowReassert(replacement, new_token), labels[index]);
        Check(!guard.Release(original, old_token) && !guard.Release(replacement, old_token) &&
            !guard.AllowReassert(original, old_token) && !guard.IsReleased(replacement, new_token) &&
            guard.Track(replacement) == new_token,
            "late release/reassert from a stale callback cannot poison its HWND replacement");
    }
}

void DestroyThenIdenticalWindowGetsNewGeneration() {
    ExplorerTakeoverGuard guard;
    const ExplorerWindowIdentity window{0x300, 30, 31, 3000};
    const auto destroyed_token = guard.Track(window);
    for (unsigned attempt = 0; attempt < ExplorerTakeoverGuard::kMaxReassertions; ++attempt)
        guard.AllowReassert(window, destroyed_token);
    guard.Release(window, destroyed_token);
    guard.Forget(window.window);
    Check(!guard.IsCurrent(window, destroyed_token) && !guard.IsReleased(window, destroyed_token) &&
        !guard.Release(window, destroyed_token) && !guard.AllowReassert(window, destroyed_token),
        "DESTROY forgets the generation before delayed callbacks arrive");

    const auto recreated_token = guard.Track(window);
    Check(recreated_token && recreated_token != destroyed_token && guard.IsCurrent(window, recreated_token) &&
        !guard.IsReleased(window, recreated_token) && guard.AllowReassert(window, recreated_token) &&
        !guard.Release(window, destroyed_token) && !guard.IsReleased(window, recreated_token),
        "same HWND PID thread and process after DESTROY starts fresh without inheriting release or retries");
}

void RepeatedStyleChangesHaveFiniteBudget() {
    ExplorerTakeoverGuard guard;
    const ExplorerWindowIdentity window{0x400, 40, 41, 4000};
    const auto token = guard.Track(window);
    Check(ExplorerTakeoverGuard::kMaxReassertions == 3,
        "Explorer style-reset recovery permits exactly three reassertions");
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        Check(guard.Track(window) == token && guard.AllowReassert(window, token),
            "repeated SHOW keeps the generation and spends one recovery attempt");
    }
    Check(!guard.AllowReassert(window, token) && !guard.AllowReassert(window, token),
        "further style changes cannot replenish the exhausted recovery budget");
    Check(guard.Release(window, token), "exhausted style recovery releases the Explorer window");
    const auto late_token = guard.Track(window);
    Check(late_token == token && guard.IsReleased(window, late_token) &&
        !guard.AllowReassert(window, late_token),
        "SHOW after the retry-limit restoration cannot revive concealment");
}

void PendingWindowRecoversWithoutRegistrationEvents() {
    pulse::app::ExplorerScanSchedule schedule;
    Check(!schedule.Due(1000, false) && schedule.Due(1000, true),
        "first pending source requests a scan while an idle watcher does not poll");
    schedule.Scanned(1000);
    Check(!schedule.Due(1499, true),
        "a hidden source does not rescan before the 500ms recovery interval");
    Check(schedule.Due(1500, true),
        "a source missing registration events rescans at the 500ms boundary");
    schedule.Scanned(1500);
    Check(!schedule.Due(1999, true) && schedule.Due(2000, true),
        "a completed scan resets the recovery interval for the still-pending source");
    Check(!schedule.Due(5000, false) && schedule.Due(5000, true),
        "a resolved source stops background scans and new pending work can resume them");
}

} // namespace

int main() {
    TimedOutWindowReceivesLateEvents();
    ReusedWindowRejectsOldCallbacks();
    DestroyThenIdenticalWindowGetsNewGeneration();
    RepeatedStyleChangesHaveFiniteBudget();
    PendingWindowRecoversWithoutRegistrationEvents();
    pulse::app::SourceCloseProbe source;
    source.identity_kept = true;
    source.hidden = true;
    source.selection_changed = true;
    Check(pulse::app::DecideSourceClose(source) == pulse::app::SourceCloseStep::Abort,
        "root normalization never permits closing a source with multiple tabs or views");
    source.single_tab = true;
    Check(pulse::app::DecideSourceClose(source) == pulse::app::SourceCloseStep::CloseAndSelect,
        "late reveal selection still follows the acknowledged single-tab source into Pulse");
    source.selection_changed = false;
    Check(pulse::app::DecideSourceClose(source) == pulse::app::SourceCloseStep::Wait,
        "hidden empty source still waits for the caller's delayed file selection");
    source.view_age_ms = pulse::app::kExplorerLateSelectionMs;
    Check(pulse::app::DecideSourceClose(source) == pulse::app::SourceCloseStep::Close,
        "acknowledged single-tab source can close after its late-selection grace period");
    std::printf("SUMMARY failures=%d\n", failures);
    return failures ? 1 : 0;
}
