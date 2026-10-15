#pragma once

#include "../ui/release_note_view.h"

#include <windows.h>
#include <string>
#include <utility>
#include <vector>

namespace pulse::app {

using AboutRow = std::pair<std::wstring, std::wstring>;  // label, value

inline constexpr const wchar_t* kPulseHomepage = L"https://github.com/chenzai666/pulse";
inline constexpr const wchar_t* kPulseReleasesPage = L"https://github.com/chenzai666/pulse/releases";
// Other free, open-source apps by the same author, recommended on the About page.
inline constexpr const wchar_t* kLumenPdfHomepage = L"https://github.com/jimmgreen/LumenPDF";
inline constexpr const wchar_t* kLumaShotHomepage = L"https://github.com/jimmgreen/LumaShot";

// Release notes embedded at build time (newest first), in the effective UI
// language (English uses <version>.en.md when present); parsed once per language.
const std::vector<ui::ReleaseNoteView>& EmbeddedReleaseNotes();

// Label/value rows for Settings > About (version, build, Windows, location, ...).
std::vector<AboutRow> BuildAboutRows(bool index_service, bool index_installed, float scale);

// Plain-text block suitable for bug reports.
std::wstring AboutRowsText(const std::vector<AboutRow>& rows);

bool CopyTextToClipboard(HWND owner, const std::wstring& text);

} // namespace pulse::app
