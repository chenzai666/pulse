#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pulse::app::folder_link {

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

// This policy only decides *when* a candidate is eligible for inspection.
// It never validates window controls or sends messages. The caller must
// independently revalidate the identities, foreground, integrity and controls
// immediately before every message, and abandon on any uncertainty.
class Transition {
public:
    void OtherForeground() noexcept { Reset(); }

    void DialogForeground(const WindowIdentity& dialog) {
        if ((phase_ == Phase::Dialog || phase_ == Phase::Returned) &&
            dialog.valid() && dialog == dialog_) return;
        // A dialog may open for the first time after Pulse was foreground.
        // Once an existing dialog was observed, return only to that identity.
        if (phase_ == Phase::Pulse && dialog.valid() && (!dialog_.valid() || dialog == dialog_)) {
            dialog_ = dialog;
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
        if (phase_ == Phase::Pulse && pulse.valid() && pulse == pulse_ && !folder.empty()) return;
        if ((phase_ != Phase::Empty && phase_ != Phase::Dialog) || !pulse.valid() || folder.empty()) {
            Reset();
            return;
        }
        pulse_ = pulse;
        initial_folder_ = std::move(folder);
        phase_ = Phase::Pulse;
    }

    // Consumed once, including on failure; callers should resolve and check
    // the final folder before returning to this function.
    bool Consume(const WindowIdentity& dialog, const WindowIdentity& pulse,
                 std::wstring_view final_folder) noexcept {
        const bool eligible = phase_ == Phase::Returned && dialog.valid() && pulse.valid() &&
            dialog == dialog_ && pulse == pulse_ && !final_folder.empty();
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
    }

    Phase phase_ = Phase::Empty;
    WindowIdentity dialog_{};
    WindowIdentity pulse_{};
    std::wstring initial_folder_;
};

} // namespace pulse::app::folder_link
