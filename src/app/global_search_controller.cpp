#include "global_search_controller.h"
#include "global_search_handoff.h"
#include "app_internal.h"
#include "tray_reveal.h"
#include "listary_search_entry.h"
#include "../common/edition.h"
#include "../common/localization.h"

namespace pulse {
namespace {
// Test instances leave the hotkey to the installed Pulse. Selftest builds can
// opt in (PULSE_TEST_GLOBAL_HOTKEY) to drive the popup with the hotkey stored
// in their own isolated data dir.
bool HotkeyAllowed(const AppState& s) {
    if (!s.isolatedTest) return true;
#ifdef PULSE_WITH_SELFTEST
    return GetEnvironmentVariableW(L"PULSE_TEST_GLOBAL_HOTKEY", nullptr, 0) > 0;
#else
    return false;
#endif
}
}

void ApplyGlobalSearchSettings(AppState& s) {
    if (!HotkeyAllowed(s)) return;
    const bool enabled = s.appPrefs.global_search_enabled;
    const bool ok = s.globalSearchHotkey.Update(ServiceWindow(s), enabled,
        s.appPrefs.global_search_modifiers, s.appPrefs.global_search_key);
    s.settings.SetGlobalSearchError(ok ? std::wstring{} : l10n::Get(
        s.globalSearchHotkey.Error() == ERROR_HOTKEY_ALREADY_REGISTERED
            ? l10n::StringId::GlobalSearchConflict : l10n::StringId::GlobalSearchRegisterFailed));
    if (!enabled || !ok) {
        CancelGlobalSearchForWindow(s);
        s.globalSearchWindow.Hide();
    }
    s.tray_controller.SetVisible(WantsTrayIcon(s, s.hidden_to_tray));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key) {
    if (!s.settings.global_search_hotkey_capturing()) return false;
    if (!IsSettingsTab(ActiveTab(s))) {
        s.settings.CancelGlobalSearchHotkeyCapture();
        return false;
    }
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
    const bool consumed = s.settings.CaptureGlobalSearchHotkey(key, modifiers);
    if (consumed) InvalidateRect(s.hwnd, nullptr, FALSE);
    return consumed;
}

namespace {
void ToggleNativeGlobalSearch(AppState& s) {
    if (s.globalSearchWindow.Visible()) {
        s.globalSearchWindow.Hide();
        return;
    }
    std::wstring folder;
    s.globalSearchWindow.SetAppearance(s.darkMode, ui::WindowEffectFromId(s.appPrefs.window_effect),
        s.appPrefs.background_image, s.accentColor);
    if (const auto* tab = ActiveTab(s); tab && !fs::IsVirtualPath(tab->current_path)) folder = tab->current_path;
    // The search window is process-wide and outlives the window that opened
    // it (M4b): continue in the most recent window when the handoff happens.
    s.globalSearchWindow.SetHandoffHandler([core = &s.core](const GlobalSearchHandoff& request) {
        if (AppState* window = core->windows.MostRecent()) ContinueGlobalSearchInMain(*window, request);
    });
    if (!s.globalSearchWindow.Show(s.hwnd, s.darkMode, s.scale, folder, s.appPrefs.search_pinyin)) {
        RevealWindow(s);
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::GlobalSearch),
            l10n::Get(l10n::StringId::GlobalSearchOpenFailed));
    }
}
}

void ToggleGlobalSearch(AppState& s) {
    if (!s.appPrefs.global_search_enabled || !s.globalSearchHotkey.Registered()) return;
    if (edition::kCustom) {
        OpenListarySearch(s);
        return;
    }
    ToggleNativeGlobalSearch(s);
}

void OpenListarySearch(AppState& s) {
    if (!s.isolatedTest && !s.safeMode && QueueListaryGlobalSearch(s.worker, s.hwnd)) return;
    RevealWindow(s);
    s.notification_toast.ShowError(s.hwnd, L"Listary",
        l10n::Pick(L"无法打开 Listary。请确认已安装并启用 Listary 6.3 或更高版本。",
            L"Unable to open Listary. Install and enable Listary 6.3 or later."));
}

void HandleGlobalSearchResult(AppState& s, WPARAM token) {
    bool opened = false;
    if (!TakeListaryGlobalSearchResult(s.hwnd, token, opened)) return;
    if (edition::kCustom) {
        if (!opened) {
            RevealWindow(s);
            s.notification_toast.ShowError(s.hwnd, L"Listary",
                l10n::Pick(L"无法打开 Listary。请确认已安装并启用 Listary 6.3 或更高版本。",
                    L"Unable to open Listary. Install and enable Listary 6.3 or later."));
        }
        return;
    }
    if (!s.appPrefs.global_search_enabled || !s.globalSearchHotkey.Registered()) return;
    if (opened) s.globalSearchWindow.Hide();
    else ToggleNativeGlobalSearch(s);
}

void CancelGlobalSearchForWindow(AppState& s) {
    CancelListaryGlobalSearch(s.hwnd);
}

void ShutdownGlobalSearch(AppState& s) {
    CancelGlobalSearchForWindow(s);
    s.globalSearchHotkey.Reset();
    s.globalSearchWindow.Shutdown();
}
}
