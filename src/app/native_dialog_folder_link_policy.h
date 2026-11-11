#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace pulse::app::folder_link {

enum class AddressEditorState { Pending, Ready, Cancelled };

// A generic UIA pane may still expose a native toolbar through MSAA. Require
// the intended point and both neighbouring samples to hit the toolbar itself.
template<class HitSelf>
bool RightToolbarBlankPoint(int width, int height, HitSelf hit_self, int& x, int& y) {
    if (width < 32 || height < 10 || width > 0xffff || height > 0xffff) return false;
    const int candidate_x = width - 16, candidate_y = height / 2;
    for (int offset : {-12, 0, 12})
        if (!hit_self(candidate_x + offset, candidate_y)) return false;
    x = candidate_x; y = candidate_y;
    return true;
}

// Shell creates its address editor asynchronously after the toolbar click.
// Keep the wait bounded and revalidate input/identity on every probe.
template<class Probe, class Pause>
bool WaitForAddressEditor(Probe probe, Pause pause) {
    for (int attempt = 0; attempt <= 25; ++attempt) {
        const auto state = probe();
        if (state == AddressEditorState::Ready) return true;
        if (state == AddressEditorState::Cancelled || attempt == 25) return false;
        pause();
    }
    return false;
}

// A handle alone is not an identity: both Windows and process IDs can be
// reused. Obtain the creation time from the process handle, not its PID.
struct WindowIdentity {
    uintptr_t window = 0;
    uint32_t process = 0;
    uint32_t thread = 0;
    uint64_t process_created = 0;

    bool valid() const noexcept { return window && process && thread && process_created; }
    bool operator==(const WindowIdentity&) const = default;
};

enum class PendingDecision {
    FocusWait, InputDebounce, IdleWait, Ready, FilenameChanged, FocusChanged,
    UnsafeInteraction, TargetChanged, Expired, FilenameWait, GuiWait, UnprotectedInput
};

enum class FilenameProbeStatus { Ready, Missing, Ambiguous, WrongIdentity, Disabled, ReadFailed };

struct PendingObservation {
    uint64_t now = 0;
    uint32_t input_tick = 0;
    uintptr_t filename_control = 0, focus = 0;
    std::wstring_view filename;
    bool target_current = false, controls_ready = false, safe_focus = false;
    bool switching_input_down = false;
    FilenameProbeStatus filename_status = FilenameProbeStatus::Ready;
    bool gui_ready = true;
};

// Keep a returning dialog pending while Shell restores its focus. Input ticks
// include harmless pointer movement and switch-key releases; guard the actual
// filename and the first established focus before restarting the idle period.
class PendingNavigation {
public:
    void Begin(uint64_t now, uintptr_t filename_control, std::wstring filename,
               uintptr_t focus = 0) {
        Reset();
        queued_at_ = now;
        filename_control_ = filename_control;
        filename_ = std::move(filename);
        focus_ = focus;
        have_filename_ = true;
    }

    void BeginBaseline(uint64_t now, uint32_t input_tick) {
        Reset();
        queued_at_ = now;
        baseline_input_ = input_tick;
    }

    PendingDecision Observe(const PendingObservation& value) {
        if (!value.target_current) return PendingDecision::TargetChanged;
        if (value.now < queued_at_ || value.now - queued_at_ >= 1200) return PendingDecision::Expired;
        if (!have_filename_ && value.input_tick != baseline_input_) return PendingDecision::UnprotectedInput;
        if (value.gui_ready && !value.safe_focus) return PendingDecision::UnsafeInteraction;
        if (value.gui_ready) {
            if (focus_ && value.focus != focus_) return PendingDecision::FocusChanged;
            if (value.focus) focus_ = value.focus;
        }
        if (value.filename_status != FilenameProbeStatus::Ready) {
            if (have_filename_ || value.filename_status == FilenameProbeStatus::Ambiguous ||
                value.filename_status == FilenameProbeStatus::WrongIdentity) return PendingDecision::FilenameChanged;
            have_idle_ = false;
            return PendingDecision::FilenameWait;
        }
        if (!have_filename_) {
            if (!value.filename_control) return PendingDecision::FilenameChanged;
            filename_control_ = value.filename_control;
            filename_ = value.filename;
            have_filename_ = true;
        }
        if (!filename_control_ || value.filename_control != filename_control_ || value.filename != filename_)
            return PendingDecision::FilenameChanged;
        if (!value.gui_ready) { have_idle_ = false; return PendingDecision::GuiWait; }
        if (!value.safe_focus) return PendingDecision::UnsafeInteraction;
        if (focus_ && value.focus != focus_) return PendingDecision::FocusChanged;
        if (!value.focus) {
            have_idle_ = false;
            return PendingDecision::FocusWait;
        }
        focus_ = value.focus;
        if (value.switching_input_down) {
            have_idle_ = false;
            return PendingDecision::IdleWait;
        }
        if (!have_idle_ || value.input_tick != input_tick_) {
            input_tick_ = value.input_tick;
            idle_at_ = value.now;
            have_idle_ = true;
            return PendingDecision::InputDebounce;
        }
        if (!value.controls_ready || value.now - idle_at_ < 150) return PendingDecision::IdleWait;
        return PendingDecision::Ready;
    }

    void Reset() {
        queued_at_ = idle_at_ = 0;
        filename_control_ = focus_ = 0;
        input_tick_ = 0;
        baseline_input_ = 0;
        have_idle_ = false;
        have_filename_ = false;
        filename_.clear();
    }
    uint32_t input_tick() const noexcept { return input_tick_; }

private:
    uint64_t queued_at_ = 0, idle_at_ = 0;
    uintptr_t filename_control_ = 0, focus_ = 0;
    uint32_t input_tick_ = 0, baseline_input_ = 0;
    bool have_idle_ = false, have_filename_ = false;
    std::wstring filename_;
};

// WinEvent callbacks can arrive after the foreground has already changed.
// Preserve recent event order without treating historical windows as current.
class ForegroundEventOrder {
public:
    bool Accept(uint32_t now, uint32_t event_time) noexcept {
        const int32_t age = static_cast<int32_t>(now - event_time);
        if (age < 0 || age > 500 ||
            (have_event_ && static_cast<int32_t>(event_time - last_event_) < 0)) return false;
        last_event_ = event_time;
        have_event_ = true;
        return true;
    }

private:
    uint32_t last_event_ = 0;
    bool have_event_ = false;
};

// An accepted Pulse event can arrive after the already observed dialog has
// returned. Reconcile only that exact current target; this snapshot is not a
// new WinEvent and must never advance ForegroundEventOrder.
inline bool CanReconcileForegroundReturn(uint32_t now, uint32_t source_event_time,
    const WindowIdentity& observed_dialog, const WindowIdentity& current_dialog,
    bool source_is_current, bool marker_valid) noexcept {
    const int32_t age = static_cast<int32_t>(now - source_event_time);
    return age >= 0 && age <= 500 && !source_is_current && marker_valid &&
        observed_dialog.valid() && current_dialog.valid() && observed_dialog == current_dialog;
}

// This policy only decides *when* a candidate is eligible for inspection.
// It never validates window controls or sends messages. The caller must
// independently revalidate the identities, foreground, integrity and controls
// immediately before every message, and abandon on any uncertainty.
class Transition {
public:
    void OtherForeground() noexcept { Reset(); }

    // Taskbar previews and other application windows may sit between Pulse
    // and the already observed dialog. Retain only that exact return target.
    // A first, unknown dialog still requires the direct Pulse transition.
    void InterveningForeground(uint64_t now = 0) noexcept {
        if (phase_ == Phase::Dialog && dialog_.valid()) return;
        if (phase_ == Phase::Pulse && dialog_.valid()) {
            if (!pulse_departed_at_) pulse_departed_at_ = now;
            return;
        }
        Reset();
    }

    void DialogForeground(const WindowIdentity& dialog, uint64_t now = 0) {
        if ((phase_ == Phase::Dialog || phase_ == Phase::Returned) &&
            dialog.valid() && dialog == dialog_) return;
        // A dialog may open for the first time after Pulse was foreground.
        // Once an existing dialog was observed, return only to that identity.
        if (phase_ == Phase::Pulse && dialog.valid() && (!dialog_.valid() || dialog == dialog_)) {
            dialog_ = dialog;
            if (!pulse_departed_at_) pulse_departed_at_ = now;
            phase_ = Phase::Returned;
            return;
        }
        Reset();
        if (dialog.valid()) {
            dialog_ = dialog;
            phase_ = Phase::Dialog;
        }
    }

    void PulseForeground(const WindowIdentity& pulse, std::wstring folder) {
        if (phase_ == Phase::Pulse && pulse.valid() && pulse == pulse_ && !folder.empty()) {
            pulse_departed_at_ = 0;
            return;
        }
        if ((phase_ != Phase::Empty && phase_ != Phase::Dialog) || !pulse.valid() || folder.empty()) {
            Reset();
            return;
        }
        pulse_ = pulse;
        initial_folder_ = std::move(folder);
        pulse_departed_at_ = 0;
        phase_ = Phase::Pulse;
    }

    // Consumed once, including on failure; callers should resolve and check
    // the final folder before returning to this function.
    bool Consume(const WindowIdentity& dialog, const WindowIdentity& pulse,
                 std::wstring_view final_folder, uint64_t now = 0) noexcept {
        const bool eligible = phase_ == Phase::Returned && dialog.valid() && pulse.valid() &&
            dialog == dialog_ && pulse == pulse_ && !final_folder.empty() &&
            (!pulse_departed_at_ || (now >= pulse_departed_at_ && now - pulse_departed_at_ <= 30000));
        Reset();
        return eligible;
    }

    std::wstring_view initial_folder() const noexcept { return initial_folder_; }

private:
    enum class Phase { Empty, Dialog, Pulse, Returned };
    void Reset() noexcept {
        phase_ = Phase::Empty;
        dialog_ = {};
        pulse_ = {};
        initial_folder_.clear();
        pulse_departed_at_ = 0;
    }

    Phase phase_ = Phase::Empty;
    WindowIdentity dialog_{};
    WindowIdentity pulse_{};
    std::wstring initial_folder_;
    uint64_t pulse_departed_at_ = 0;
};

} // namespace pulse::app::folder_link
