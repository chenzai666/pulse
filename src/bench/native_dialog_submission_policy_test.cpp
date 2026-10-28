#include "../app/native_dialog_submission_policy.h"
#include <cstdio>

int main() {
    using namespace pulse::app;
    int failures = 0;
    const auto check = [&](bool passed, const char* label) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
        if (!passed) ++failures;
    };
    NativeSubmissionPolicy success;
    check(success.Begin() && !success.Begin() && !success.Begin(true), "one in-flight command rejects duplicate confirm and cancel");
    const auto accepted = success.Complete(NativeSubmissionOutcome::Closed, true);
    check(accepted.accepted && !accepted.retry_allowed && !success.Begin() && !success.Begin(true), "closed source acknowledges once and rejects all later commands");
    NativeSubmissionPolicy retry;
    check(retry.Begin(), "first attempt starts");
    const auto rejected = retry.Complete(NativeSubmissionOutcome::Rejected, false);
    check(!rejected.accepted && rejected.retry_allowed && retry.Begin(), "definite rejection before posting permits corrected selection");
    const auto uncertain = retry.Complete(NativeSubmissionOutcome::TimedOut, true);
    check(!uncertain.accepted && !uncertain.retry_allowed && !retry.Begin() && retry.Begin(true), "posted timeout locks affirmative command but permits explicit cancellation");
    const auto cancelled = retry.Complete(NativeSubmissionOutcome::Closed, true);
    check(cancelled.accepted && !retry.Begin(), "explicit cancellation completes without another affirmative command");
    NativeSubmissionPolicy validation;
    check(validation.Begin(), "validation attempt starts");
    const auto blocked = validation.Complete(NativeSubmissionOutcome::Validation, true);
    check(!blocked.accepted && !blocked.retry_allowed && !validation.Begin() && validation.Begin(true), "native validation cannot trigger implicit retry or overwrite acceptance");
    const auto cancel_failed = validation.Complete(NativeSubmissionOutcome::Rejected, false);
    check(!cancel_failed.accepted && !cancel_failed.retry_allowed && !validation.Begin() && validation.Begin(true), "failed cancellation retains lock while permitting a deliberate cancel retry");
    NativeSubmissionPolicy shutdown;
    check(shutdown.Begin(), "shutdown attempt starts");
    const auto stopped = shutdown.Complete(NativeSubmissionOutcome::Cancelled, false);
    check(!stopped.accepted && !stopped.retry_allowed && !shutdown.Begin(), "shutdown never produces a successful submit or permits replay");
    check(!shutdown.Complete(NativeSubmissionOutcome::Closed, true).accepted, "unsolicited completion cannot acknowledge an unstarted operation");
    return failures ? 1 : 0;
}
