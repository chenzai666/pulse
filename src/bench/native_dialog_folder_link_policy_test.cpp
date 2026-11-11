#include "../app/native_dialog_folder_link_policy.h"

#include <cstdio>
#include <string>

using pulse::app::folder_link::Transition;
using pulse::app::folder_link::WindowIdentity;
using pulse::app::folder_link::ForegroundEventOrder;

namespace {
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label);
    if (!value) ++failures;
}

constexpr WindowIdentity dialog{11, 21, 31, 41};
constexpr WindowIdentity pulse_window{12, 22, 32, 42};

void Arm(Transition& transition) {
    transition.DialogForeground(dialog);
    transition.PulseForeground(pulse_window, L"C:\\initial");
    transition.DialogForeground(dialog);
}
}

int main() {
    using pulse::app::folder_link::RightToolbarBlankPoint;
    int x = -1, y = -1;
    Check(RightToolbarBlankPoint(503, 35, [](int, int) { return true; }, x, y) && x == 487 && y == 17,
          "generic UIA pane can use verified native toolbar blank area");
    Check(!RightToolbarBlankPoint(503, 35, [](int px, int) { return px != 487; }, x, y),
          "toolbar child at intended click point is rejected");
    Check(!RightToolbarBlankPoint(503, 35, [](int px, int) { return px != 499; }, x, y),
          "toolbar child neighbouring intended click point is rejected");
    Check(!RightToolbarBlankPoint(20, 35, [](int, int) { return true; }, x, y),
          "narrow toolbar has no safe fallback click point");
    using pulse::app::folder_link::AddressEditorState;
    using pulse::app::folder_link::WaitForAddressEditor;
    int probes = 0, pauses = 0;
    Check(WaitForAddressEditor([&] { ++probes; return AddressEditorState::Ready; }, [&] { ++pauses; }) &&
          probes == 1 && pauses == 0, "ready address editor does not wait");
    probes = pauses = 0;
    Check(WaitForAddressEditor([&] { return ++probes == 4 ? AddressEditorState::Ready : AddressEditorState::Pending; },
          [&] { ++pauses; }) && probes == 4 && pauses == 3, "asynchronously created editor is awaited");
    probes = pauses = 0;
    Check(!WaitForAddressEditor([&] { ++probes; return AddressEditorState::Pending; }, [&] { ++pauses; }) &&
          probes == 26 && pauses == 25, "unavailable editor has a bounded wait");
    probes = pauses = 0;
    Check(!WaitForAddressEditor([&] { return ++probes == 2 ? AddressEditorState::Cancelled : AddressEditorState::Pending; },
          [&] { ++pauses; }) && probes == 2 && pauses == 1, "new user input or changed identity cancels editor wait");
    using pulse::app::folder_link::PendingNavigation;
    using pulse::app::folder_link::PendingObservation;
    using pulse::app::folder_link::PendingDecision;
    PendingNavigation pending;
    PendingObservation observed{1000, 17170015, 101, 0, L"unchanged.txt", true, true, true, false};
    pending.Begin(1000, 101, L"unchanged.txt");
    Check(pending.Observe(observed) == PendingDecision::FocusWait,
          "foreground dialog with no focus remains pending instead of consuming navigation");
    observed.now = 1141; observed.input_tick = 17170156;
    Check(pending.Observe(observed) == PendingDecision::FocusWait,
          "141ms switch input change preserves pending dialog while focus is absent");
    observed.now = 1368; observed.focus = 101;
    Check(pending.Observe(observed) == PendingDecision::InputDebounce,
          "first safe filename focus begins the bounded idle wait");
    observed.now = 1517;
    Check(pending.Observe(observed) == PendingDecision::IdleWait,
          "restored focus still waits for 150ms of stable input");
    observed.now = 1518;
    Check(pending.Observe(observed) == PendingDecision::Ready && pending.input_tick() == 17170156,
          "delayed native focus dispatches with the latest stable input tick");
    observed.now = 1520; observed.input_tick = 17170157;
    Check(pending.Observe(observed) == PendingDecision::InputDebounce,
          "harmless pointer input restarts idle wait without permanently cancelling");
    observed.now = 1669;
    Check(pending.Observe(observed) == PendingDecision::IdleWait, "pointer debounce cannot dispatch early");
    observed.now = 1670;
    Check(pending.Observe(observed) == PendingDecision::Ready, "unchanged filename and focus permit dispatch after pointer debounce");
    observed.filename = L"typed.txt"; observed.input_tick = 17170158;
    Check(pending.Observe(observed) == PendingDecision::FilenameChanged,
          "typing a filename cancels before input can be treated as harmless debounce");
    observed.filename = L"unchanged.txt"; observed.filename_control = 102;
    Check(pending.Observe(observed) == PendingDecision::FilenameChanged, "replacement filename control is rejected");
    observed.filename_control = 101; observed.focus = 102;
    Check(pending.Observe(observed) == PendingDecision::FocusChanged, "changing established focus cancels navigation");
    observed.focus = 0;
    Check(pending.Observe(observed) == PendingDecision::FocusChanged, "losing established focus cancels navigation");
    observed.focus = 101; observed.target_current = false;
    Check(pending.Observe(observed) == PendingDecision::TargetChanged, "switching away from exact dialog cancels navigation");
    observed.target_current = true; observed.safe_focus = false;
    Check(pending.Observe(observed) == PendingDecision::UnsafeInteraction, "menu capture and address editing remain rejected");
    observed.safe_focus = true; observed.now = 2199; observed.switching_input_down = true;
    Check(pending.Observe(observed) == PendingDecision::IdleWait, "held switch keys or mouse buttons can only wait");
    observed.now = 2200; observed.switching_input_down = false;
    Check(pending.Observe(observed) == PendingDecision::Expired, "1200ms deadline is never extended by input or focus waits");
    pending.Begin(3000, 101, L"unchanged.txt", 101);
    observed.now = 3000; observed.controls_ready = false;
    Check(pending.Observe(observed) == PendingDecision::InputDebounce, "already established safe focus begins idle wait");
    observed.now = 3150;
    Check(pending.Observe(observed) == PendingDecision::IdleWait, "missing address controls keep navigation pending");
    observed.controls_ready = true;
    Check(pending.Observe(observed) == PendingDecision::Ready, "ready address controls permit guarded dispatch");
    pending.Begin(4000, 101, L"unchanged.txt", 101);
    observed.now = 4000; observed.focus = 102;
    Check(pending.Observe(observed) == PendingDecision::FocusChanged, "queued focus baseline detects change before first timer");
    pending.Reset(); observed.focus = 101;
    Check(pending.Observe(observed) == PendingDecision::Expired, "reset pending state cannot resume an old wait");
    pending.Begin(5000, 101, L"unchanged.txt", 101);
    observed.now = 5000; observed.switching_input_down = true;
    Check(pending.Observe(observed) == PendingDecision::IdleWait, "held switch input does not start idle accumulation");
    observed.now = 5300;
    Check(pending.Observe(observed) == PendingDecision::IdleWait, "long held switch input still cannot dispatch");
    observed.now = 5301; observed.switching_input_down = false;
    Check(pending.Observe(observed) == PendingDecision::InputDebounce, "switch release starts a fresh 150ms idle wait");
    observed.now = 5450;
    Check(pending.Observe(observed) == PendingDecision::IdleWait, "time spent holding a switch key is excluded from idle");
    observed.now = 5451;
    Check(pending.Observe(observed) == PendingDecision::Ready, "released switch input dispatches only after a full stable idle period");
    using pulse::app::folder_link::FilenameProbeStatus;
    pending.BeginBaseline(6000, 90);
    observed = {6000, 90, 101, 0, L"baseline.txt", true, true, true, false};
    observed.gui_ready = false;
    Check(pending.Observe(observed) == PendingDecision::GuiWait,
          "filename is retained when initial GUI query is temporarily unavailable");
    observed.now = 6050; observed.gui_ready = true; // Hidden in-dialog focus is normalized to zero by the native probe.
    Check(pending.Observe(observed) == PendingDecision::FocusWait,
          "hidden address focus does not become an established focus baseline");
    observed.now = 6100; observed.focus = 0;
    Check(pending.Observe(observed) == PendingDecision::FocusWait,
          "hidden focus followed by no focus keeps the original baseline pending");
    observed.now = 6150; observed.focus = 101; observed.input_tick = 91;
    Check(pending.Observe(observed) == PendingDecision::InputDebounce,
          "first visible safe focus starts a fresh idle interval after protected switch input");
    observed.now = 6300;
    Check(pending.Observe(observed) == PendingDecision::Ready,
          "staged filename and visible focus permit dispatch after stable idle");
    pending.BeginBaseline(7000, 100);
    observed = {7000, 100, 101, 0, L"baseline.txt", true, true, true, false};
    observed.gui_ready = false;
    Check(pending.Observe(observed) == PendingDecision::GuiWait, "staged filename locks before GUI recovery");
    observed.now = 7050; observed.filename = L"edited.txt";
    Check(pending.Observe(observed) == PendingDecision::FilenameChanged,
          "editing retained filename during GUI initialization cancels");
    observed.filename = L"baseline.txt"; observed.filename_control = 102;
    Check(pending.Observe(observed) == PendingDecision::FilenameChanged,
          "replacing retained filename control during GUI initialization cancels");
    pending.BeginBaseline(8000, 110);
    observed = {8000, 110, 0, 101, L"", true, true, true, false, FilenameProbeStatus::Missing, true};
    Check(pending.Observe(observed) == PendingDecision::FilenameWait,
          "missing initial filename control can appear within the original deadline");
    observed.now = 8050; observed.filename_status = FilenameProbeStatus::Ready;
    observed.filename_control = 101; observed.filename = L"baseline.txt";
    Check(pending.Observe(observed) == PendingDecision::InputDebounce,
          "unchanged input permits first filename baseline after a missing-control wait");
    pending.BeginBaseline(8500, 115);
    observed = {8500, 115, 0, 101, L"", true, true, true, false, FilenameProbeStatus::Missing, true};
    Check(pending.Observe(observed) == PendingDecision::FilenameWait,
          "safe visible focus locks independently while the first filename control is unavailable");
    observed.now = 8550; observed.focus = 102;
    Check(pending.Observe(observed) == PendingDecision::FocusChanged,
          "changing independently retained focus cancels even before filename initialization");
    pending.BeginBaseline(9000, 120);
    observed = {9000, 120, 0, 0, L"", true, false, true, false, FilenameProbeStatus::ReadFailed, false};
    Check(pending.Observe(observed) == PendingDecision::FilenameWait, "transient first filename read failure remains pending");
    observed.now = 9050; observed.input_tick = 121; observed.filename_status = FilenameProbeStatus::Ready;
    observed.filename_control = 101; observed.filename = L"typed-during-read.txt";
    Check(pending.Observe(observed) == PendingDecision::UnprotectedInput,
          "post-read input change prevents newly typed text from becoming an initial baseline");
    pending.BeginBaseline(10000, 130);
    observed = {10000, 130, 0, 0, L"", true, false, true, false, FilenameProbeStatus::Disabled, false};
    Check(pending.Observe(observed) == PendingDecision::FilenameWait, "temporarily disabled initial filename control is awaited");
    observed.now = 11200;
    Check(pending.Observe(observed) == PendingDecision::Expired, "staged baseline retries never extend the 1200ms total deadline");
    pending.BeginBaseline(12000, 140);
    observed = {12000, 140, 101, 101, L"baseline.txt", true, true, false, false};
    Check(pending.Observe(observed) == PendingDecision::UnsafeInteraction,
          "visible address editing and external focus remain unsafe during initialization");
    observed.filename_status = FilenameProbeStatus::Missing;
    Check(pending.Observe(observed) == PendingDecision::UnsafeInteraction,
          "capture or menu is rejected even while filename control is missing");
    observed.safe_focus = true; observed.filename_status = FilenameProbeStatus::Ambiguous;
    Check(pending.Observe(observed) == PendingDecision::FilenameChanged, "ambiguous initial filename controls are never awaited");
    observed.filename_status = FilenameProbeStatus::WrongIdentity;
    Check(pending.Observe(observed) == PendingDecision::FilenameChanged, "wrong initial control identity is immediately rejected");
    observed.target_current = false;
    Check(pending.Observe(observed) == PendingDecision::TargetChanged, "staged baseline cannot survive loss of the exact foreground target");
    Transition t;
    using pulse::app::folder_link::CanReconcileForegroundReturn;
    ForegroundEventOrder reconciled_order;
    Check(reconciled_order.Accept(1000, 1000), "real dialog callback is accepted before delayed source callback");
    t.DialogForeground(dialog, 1000);
    Check(reconciled_order.Accept(1010, 1000), "same-tick delayed Pulse callback preserves real event ordering");
    t.PulseForeground(pulse_window, L"C:\\final");
    const bool reconcile = CanReconcileForegroundReturn(1010, 1000, dialog, dialog, false, true);
    if (reconcile) t.DialogForeground(dialog, 1010);
    Check(reconcile && t.Consume(dialog, pulse_window, L"C:\\final", 1010),
          "dialog then historical Pulse callback reconciles exact current dialog return");
    Check(reconciled_order.Accept(1010, 1005),
          "current foreground snapshot does not fabricate a newer WinEvent timestamp");
    Check(!CanReconcileForegroundReturn(1010, 1000, {}, dialog, false, true),
          "unknown dialog is never reconciled from a historical source callback");
    Check(!CanReconcileForegroundReturn(1010, 1000, dialog, WindowIdentity{13, 23, 33, 43}, false, true),
          "replacement current dialog cannot inherit an observed return target");
    Check(!CanReconcileForegroundReturn(1010, 1000, dialog, {}, false, true),
          "non-dialog foreground does not reconcile a return");
    Check(!CanReconcileForegroundReturn(1010, 1000, dialog, dialog, true, true),
          "still-current Pulse source waits for a normal dialog foreground callback");
    Check(!CanReconcileForegroundReturn(1501, 1000, dialog, dialog, false, true),
          "source event older than 500ms cannot reconcile current dialog");
    Check(!CanReconcileForegroundReturn(999, 1000, dialog, dialog, false, true),
          "future source timestamp cannot reconcile current dialog");
    Check(!CanReconcileForegroundReturn(1010, 1000, dialog, dialog, false, false),
          "revoked marker prevents historical source reconciliation");
    Check(CanReconcileForegroundReturn(1500, 1000, dialog, dialog, false, true),
          "source event at the accepted 500ms boundary remains eligible");
    Check(CanReconcileForegroundReturn(10, 0xfffffff0u, dialog, dialog, false, true),
          "historical source freshness handles tick count wrap");
    t.DialogForeground(dialog, 2000);
    t.PulseForeground(pulse_window, L"C:\\final");
    t.DialogForeground(dialog, 2010);
    Check(t.Consume(dialog, pulse_window, L"C:\\final", 2010),
          "normal ordered Pulse then dialog return still follows without reconciliation");
    t.DialogForeground(dialog, 100);
    t.InterveningForeground(110); // Taskbar while switching away from dialog.
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.InterveningForeground(200); // Taskbar preview or another application.
    t.InterveningForeground(250);
    t.DialogForeground(dialog, 300);
    Check(t.Consume(dialog, pulse_window, L"C:\\final", 300),
          "return through taskbar or application preserves exact observed dialog");
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.InterveningForeground(200);
    t.DialogForeground(dialog, 300);
    Check(!t.Consume(dialog, pulse_window, L"C:\\final", 300),
          "unknown first dialog is not armed across intervening applications");
    t.DialogForeground(dialog, 100);
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.InterveningForeground(200);
    t.DialogForeground(WindowIdentity{13, 23, 33, 43}, 300);
    Check(!t.Consume(WindowIdentity{13, 23, 33, 43}, pulse_window, L"C:\\final", 300),
          "intervening windows never redirect pending folder into another dialog");
    t.DialogForeground(dialog, 100);
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.InterveningForeground(200);
    t.DialogForeground(dialog, 30201);
    Check(!t.Consume(dialog, pulse_window, L"C:\\final", 30201),
          "pending folder expires thirty seconds after leaving Pulse");
    t.DialogForeground(dialog, 100);
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.InterveningForeground(600000); // Browsing Pulse itself has no time limit.
    t.DialogForeground(dialog, 600100);
    Check(t.Consume(dialog, pulse_window, L"C:\\final", 600100),
          "long Pulse browsing still follows on prompt return to original dialog");
    ForegroundEventOrder order;
    if (order.Accept(1200, 1000)) t.DialogForeground(dialog);
    if (order.Accept(1200, 1100)) t.PulseForeground(pulse_window, L"C:\\initial");
    if (order.Accept(1200, 1200)) t.DialogForeground(dialog);
    Check(t.Consume(dialog, pulse_window, L"C:\\final"), "queued rapid Pulse visit preserves dialog return transition");
    Check(!order.Accept(1200, 1150), "late older foreground callback is rejected");
    Check(!order.Accept(1200, 1201), "future foreground timestamp is rejected");
    Check(!order.Accept(1701, 1200), "foreground callback older than 500ms is rejected");
    Check(order.Accept(1700, 1200), "foreground callback at 500ms boundary remains eligible");

    ForegroundEventOrder wrap;
    Check(wrap.Accept(10, 0xfffffff0u) && wrap.Accept(10, 5), "foreground ordering survives tick count wrap");
    Check(!wrap.Accept(10, 0xfffffffcu), "older callback across tick count wrap is rejected");

    t.PulseForeground(pulse_window, L"C:\\initial");
    t.DialogForeground(dialog);
    Check(t.Consume(dialog, pulse_window, L"C:\\final"), "first dialog opened from Pulse follows the final folder");
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "first dialog transition is consumed exactly once");

    t.PulseForeground(pulse_window, L"C:\\initial");
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.DialogForeground(dialog);
    t.DialogForeground(dialog);
    Check(t.Consume(dialog, pulse_window, L"C:\\initial"), "duplicate events preserve first dialog transition");

    t.PulseForeground(pulse_window, L"C:\\initial");
    t.OtherForeground();
    t.DialogForeground(dialog);
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "intervening application cancels first dialog transition");

    Arm(t);
    Check(t.initial_folder() == L"C:\\initial", "record initial Pulse folder");
    Check(t.Consume(dialog, pulse_window, L"C:\\final"), "matching dialog and Pulse transition accepted");
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "transition is consumed exactly once");

    Arm(t);
    Check(t.Consume(dialog, pulse_window, L"C:\\initial"), "unchanged Pulse folder still triggers navigation");
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "successful attempt is consumed");

    t.DialogForeground(dialog);
    t.DialogForeground(dialog);
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.DialogForeground(dialog);
    t.DialogForeground(dialog);
    Check(t.Consume(dialog, pulse_window, L"C:\\final"), "duplicate foreground events preserve a matching transition");
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "duplicate events do not permit a second navigation");

    Arm(t);
    Check(!t.Consume(dialog, pulse_window, L""), "empty final folder is rejected");
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "failed attempt is consumed");
    Arm(t);
    Check(!t.Consume(WindowIdentity{11, 21, 31, 999}, pulse_window, L"C:\\final"),
          "reused process identity is rejected");
    Arm(t);
    Check(!t.Consume(dialog, WindowIdentity{12, 22, 99, 42}, L"C:\\final"),
          "different Pulse thread is rejected");

    t.DialogForeground(dialog);
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.OtherForeground();
    t.DialogForeground(dialog);
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "intervening application resets transition");

    t.DialogForeground(dialog);
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.DialogForeground(WindowIdentity{13, 23, 33, 43});
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "different dialog cannot consume transition");

    t.DialogForeground({});
    t.PulseForeground(pulse_window, L"C:\\initial");
    t.DialogForeground(dialog);
    Check(t.Consume(dialog, pulse_window, L"C:\\final"), "missing prior dialog permits first dialog transition");
    t.DialogForeground(dialog);
    t.PulseForeground({}, L"C:\\initial");
    t.DialogForeground(dialog);
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "invalid Pulse identity is rejected");
    t.DialogForeground(dialog);
    t.PulseForeground(pulse_window, L"");
    t.DialogForeground(dialog);
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "empty initial folder is rejected");
    std::printf("SUMMARY failures=%d\n", failures);
    return failures ? 1 : 0;
}
