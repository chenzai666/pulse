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
    Transition t;
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
