#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace pulse::ui {

constexpr uintptr_t kPickerListaryData = 0x504C5354;
constexpr size_t kPickerListaryMaxBytes = 65536;
constexpr wchar_t kPickerListaryMarker[] = L"Pulse.Picker.Listary.v1";

struct PickerListaryRequest {
    bool file = false;
    std::wstring path;
};

inline bool IsPickerListaryPath(std::wstring_view path) {
    const bool drive = path.size() >= 3 &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && path[2] == L'\\';
    const bool unc = path.starts_with(L"\\\\");
    if (!drive && !unc) return false;
    for (size_t i = 0; i < path.size(); ++i) {
        const auto ch = static_cast<uint16_t>(path[i]);
        if (ch < 32 || path[i] == L'/' || path[i] == L'"' || path[i] == L'<' ||
            path[i] == L'>' || path[i] == L'|' || path[i] == L'*' || path[i] == L'?' ||
            (path[i] == L':' && !(drive && i == 1))) return false;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (++i >= path.size()) return false;
            const auto low = static_cast<uint16_t>(path[i]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) return false;
    }
    size_t components = 0;
    for (size_t start = drive ? 3 : 2; start < path.size();) {
        const size_t end = path.find(L'\\', start);
        const auto part = path.substr(start, end == std::wstring_view::npos ? end : end - start);
        if (part.empty() || part.back() == L'.' || part.back() == L' ') return false;
        std::wstring stem(part.substr(0, part.find(L'.')));
        while (!stem.empty() && stem.back() == L' ') stem.pop_back();
        for (auto& ch : stem) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            stem == L"CONIN$" || stem == L"CONOUT$") return false;
        if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
            ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\xB9' ||
             stem[3] == L'\xB2' || stem[3] == L'\xB3')) return false;
        ++components;
        if (end == std::wstring_view::npos) break;
        start = end + 1;
    }
    return drive || components >= 2;
}

inline std::optional<PickerListaryRequest> ParsePickerListaryRequest(
    uintptr_t tag, uintptr_t operation, const void* bytes, size_t size) {
    static_assert(sizeof(wchar_t) == 2, "Listary messages use Windows UTF-16");
    if (tag != kPickerListaryData || (operation != 1 && operation != 2) || !bytes ||
        size < 4 || size > kPickerListaryMaxBytes || size % sizeof(wchar_t)) return std::nullopt;
    std::wstring path(size / sizeof(wchar_t), L'\0');
    // COPYDATA data is bounded; memcpy also handles unaligned UTF-16 buffers.
    std::memcpy(path.data(), bytes, size);
    if (path.back() != L'\0') return std::nullopt;
    path.pop_back();
    if (!IsPickerListaryPath(path)) return std::nullopt;
    return PickerListaryRequest{operation == 2, std::move(path)};
}

} // namespace pulse::ui
