#include "../app/native_dialog_folder_link_policy.h"

#include <cstdio>
#include <string>

using pulse::app::folder_link::Transition;
using pulse::app::folder_link::WindowIdentity;

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
    Transition t;
    Arm(t);
    Check(t.initial_folder() == L"C:\\initial", "record initial Pulse folder");
    Check(t.Consume(dialog, pulse_window, L"C:\\final"), "matching dialog and Pulse transition accepted");
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "transition is consumed exactly once");

    Arm(t);
    Check(t.Consume(dialog, pulse_window, L"C:\\initial"), "unchanged Pulse folder still triggers navigation");
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "successful attempt is consumed");

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
    Check(!t.Consume(dialog, pulse_window, L"C:\\final"), "invalid source dialog is rejected");
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
