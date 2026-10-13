#pragma once

#include <d2d1.h>
#include <windows.h>

#include <string>
#include <vector>

namespace pulse::ui {

// The icon tile and its colour. Auto picks Danger for `danger` specs and
// Question otherwise.
enum class ConfirmTone { Auto, Info, Question, Warning, Danger };

enum class ConfirmChoice { Cancel, Confirm, Secondary };

struct ConfirmCheckboxSpec {
    std::wstring text; // empty: no checkbox
    bool checked = false;
    bool danger_when_checked = false;
    std::wstring checked_message;
    std::wstring checked_confirm_text;
    std::wstring checked_note;
};

struct ConfirmDialogSpec {
    std::wstring title;
    std::wstring message;
    std::wstring confirm_text;
    std::wstring cancel_text;
    bool danger = false;
    // Optional middle button, e.g. "No" in a Yes / No / Cancel question.
    std::wstring secondary_text;
    // Optional list shown in a card under the message (one row per item).
    std::vector<std::wstring> items;
    std::wstring item_glyph;
    // Optional small print under the card.
    std::wstring note;
    ConfirmTone tone = ConfirmTone::Auto;
    std::wstring glyph;  // overrides the tone's icon
    // The button Enter presses at first. Destructive questions start on
    // Cancel, like MB_DEFBUTTON2, so Enter alone never confirms them.
    ConfirmChoice default_choice = ConfirmChoice::Confirm;
    ConfirmCheckboxSpec checkbox;
};

struct ConfirmDialogResult {
    ConfirmChoice choice = ConfirmChoice::Cancel;
    bool checkbox_checked = false;
};

// Returns the checkbox choice for this invocation; no preference is persisted.
ConfirmDialogResult ShowConfirmDialogWithOptions(HWND owner, const ConfirmDialogSpec& spec,
                                                 bool dark, D2D1_COLOR_F accent);

ConfirmChoice ShowConfirmDialogEx(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                                  D2D1_COLOR_F accent);

// True when the user picked the confirm button.
bool ShowConfirmDialog(HWND owner, const ConfirmDialogSpec& spec, bool dark,
                       D2D1_COLOR_F accent);

} // namespace pulse::ui
