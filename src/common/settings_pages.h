#pragma once

namespace pulse::settings {
inline constexpr int kPageCount = 6;
#ifdef PULSE_CUSTOM_EDITION
inline constexpr int kNavigationPages[] = {0, 2, 5, 3};
#else
inline constexpr int kNavigationPages[] = {0, 1, 2, 4, 5, 3};
#endif

constexpr bool PageAvailable(int page) noexcept {
    for (const int available : kNavigationPages)
        if (page == available) return true;
    return false;
}

constexpr int NormalizePage(int page) noexcept {
    if (page < 0) page = 0;
    if (page >= kPageCount) page = kPageCount - 1;
    return PageAvailable(page) ? page : 0;
}
}
