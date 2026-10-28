#pragma once

namespace pulse::app {

enum class NativeSubmissionOutcome { Closed, Rejected, TimedOut, Validation, Cancelled };
struct NativeSubmissionDecision {
    bool accepted = false;
    bool retry_allowed = false;
};

// One source dialog may receive another affirmative command only when the
// previous attempt is known to have stopped before posting any command.
class NativeSubmissionPolicy {
public:
    bool Begin(bool cancelling = false) {
        if (pending_ || finished_ || (!cancelling && affirmative_locked_)) return false;
        pending_ = true;
        cancelling_ = cancelling;
        return true;
    }
    NativeSubmissionDecision Complete(NativeSubmissionOutcome outcome, bool command_posted) {
        if (!pending_) return {};
        pending_ = false;
        if (outcome == NativeSubmissionOutcome::Closed) {
            finished_ = true;
            return {true, false};
        }
        if (cancelling_ || command_posted || outcome != NativeSubmissionOutcome::Rejected)
            affirmative_locked_ = true;
        return {false, !affirmative_locked_};
    }
private:
    bool pending_ = false, cancelling_ = false, affirmative_locked_ = false, finished_ = false;
};

} // namespace pulse::app
