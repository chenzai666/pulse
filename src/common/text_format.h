#pragma once

#include <windows.h>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>

namespace pulse::format {

enum class ByteSizeStyle { Spaced, Compact };

inline std::wstring ByteSize(uint64_t bytes, bool empty_zero = false,
                             ByteSizeStyle style = ByteSizeStyle::Spaced) {
    if (empty_zero && bytes == 0) return L"";
    static constexpr const wchar_t* spaced_units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    static constexpr const wchar_t* compact_units[] = { L"B", L"K", L"M", L"G", L"TB" };
    const auto* units = style == ByteSizeStyle::Compact ? compact_units : spaced_units;
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    constexpr size_t unit_count = 5;
    while (value >= 1024.0 && unit + 1 < unit_count) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t text[64];
    if (style == ByteSizeStyle::Compact) {
        if (unit == 0) swprintf_s(text, L"%lluB", bytes);
        else if (value >= 10.0) swprintf_s(text, L"%.0f%s", value, units[unit]);
        else swprintf_s(text, L"%.1f%s", value, units[unit]);
    } else if (unit == 0) swprintf_s(text, L"%llu %s", bytes, units[unit]);
    else swprintf_s(text, L"%.1f %s", value, units[unit]);
    return text;
}

inline std::wstring DriveCapacity(uint64_t bytes) {
    static constexpr const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB"};
    size_t unit = 0;
    uint64_t divisor = 1;
    while (bytes / divisor >= 1024 && unit + 1 < std::size(units)) {
        divisor *= 1024;
        ++unit;
    }
    if (!unit) return std::to_wstring(bytes) + L" B";
    const auto rounded = [bytes](uint64_t scale) {
        uint64_t remainder = bytes % scale;
        uint64_t hundredths = (bytes / scale) * 100;
        // Extract decimal digits separately: remainder * 100 could overflow
        // at EiB, whereas remainder * 10 stays within uint64_t.
        for (const uint64_t weight : {uint64_t{10}, uint64_t{1}}) {
            remainder *= 10;
            hundredths += (remainder / scale) * weight;
            remainder %= scale;
        }
        return hundredths + (remainder >= scale / 2 ? 1u : 0u);
    };
    uint64_t value = rounded(divisor);
    if (value == 102400 && unit + 1 < std::size(units)) {
        divisor *= 1024;
        ++unit;
        value = rounded(divisor);
    }
    std::wstring text = std::to_wstring(value / 100);
    if (const auto fraction = value % 100; fraction) {
        text += L'.';
        text += static_cast<wchar_t>(L'0' + fraction / 10);
        if (fraction % 10) text += static_cast<wchar_t>(L'0' + fraction % 10);
    }
    return text + L" " + units[unit];
}

inline std::wstring LocalFileTime(const FILETIME& time,
                                  std::wstring_view fallback = {}) {
    FILETIME local{};
    SYSTEMTIME system{};
    if (!FileTimeToLocalFileTime(&time, &local) ||
        !FileTimeToSystemTime(&local, &system)) {
        return std::wstring(fallback);
    }
    wchar_t text[48];
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u", system.wYear, system.wMonth,
               system.wDay, system.wHour, system.wMinute);
    return text;
}

// 2895851315 -> "2,895,851,315"
inline std::wstring GroupedInt(uint64_t value) {
    wchar_t digits[24];
    swprintf_s(digits, L"%llu", value);
    std::wstring out;
    const size_t len = wcslen(digits);
    for (size_t i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0) out += L',';
        out += digits[i];
    }
    return out;
}

} // namespace pulse::format
