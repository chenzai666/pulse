#include "../common/text_format.h"
#include "../ui/fluent_components.h"

#include <cstdio>

int main() {
    using namespace pulse::format;
    constexpr uint64_t kib = UINT64_C(1024), mib = kib * 1024, gib = mib * 1024;
    constexpr uint64_t tib = gib * 1024, pib = tib * 1024, eib = pib * 1024;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool passed, const char* label) {
        ++checks;
        if (!passed) ++failures;
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    };
    check(DriveCapacity(gib * 3772 / 100) == L"37.72 GiB", "two fractional GiB digits");
    check(DriveCapacity(gib * 19913 / 100) == L"199.13 GiB", "total capacity precision");
    check(DriveCapacity(gib * 6790 / 100) == L"67.9 GiB", "trailing fractional zero removed");
    check(DriveCapacity(gib * 500) == L"500 GiB", "whole GiB has no decimal suffix");
    check(DriveCapacity(0) == L"0 B", "zero bytes");
    check(DriveCapacity(1) == L"1 B" && DriveCapacity(1023) == L"1023 B", "bytes stay exact");
    check(DriveCapacity(kib) == L"1 KiB", "KiB threshold");
    check(DriveCapacity(kib + 5) == L"1 KiB", "below half-hundredth rounds down");
    check(DriveCapacity(kib + 6) == L"1.01 KiB", "above half-hundredth rounds up");
    check(DriveCapacity(3 * kib / 2) == L"1.5 KiB", "single fractional digit");
    check(DriveCapacity(kib + kib / 8) == L"1.13 KiB", "exact half-hundredth rounds up");
    check(DriveCapacity(mib * 1234 / 100) == L"12.34 MiB", "MiB capacity");
    check(DriveCapacity(tib * 250 / 100) == L"2.5 TiB", "TiB capacity");
    check(DriveCapacity(pib) == L"1 PiB", "PiB capacity");
    check(DriveCapacity(eib) == L"1 EiB", "EiB capacity");
    check(DriveCapacity(mib - 1) == L"1 MiB", "rounding carries into next IEC unit");
    check(DriveCapacity(UINT64_MAX) == L"16 EiB", "uint64 maximum does not overflow");
    check(ByteSize(gib * 3772 / 100, false, ByteSizeStyle::Compact) == L"38G",
        "existing compact file-size behavior unchanged");
    check(ByteSize(gib * 3772 / 100) == L"37.7 GB", "existing spaced file-size behavior unchanged");
    check(ByteSize(0, true).empty() && ByteSize(0) == L"0 B", "existing zero file-size behavior unchanged");
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        for (const float width : {180.0f, 240.0f, 320.0f}) {
            const auto bounds = D2D1::RectF(0, 0, width * scale,
                pulse::ui::fluent::kDriveSidebarItemHeightDip * scale);
            const auto layout = pulse::ui::fluent::LayoutDriveSidebarItem(bounds, scale);
            check(layout.icon.right < layout.name.left && layout.name.right <= bounds.right &&
                  layout.name.top == layout.icon.top && layout.name.bottom < layout.detail.top &&
                  layout.detail.bottom < layout.track.top && layout.detail.left >= bounds.left &&
                  layout.detail.right <= bounds.right && layout.detail.right > layout.detail.left &&
                  layout.track.bottom - layout.track.top == 4.0f * scale &&
                  bounds.bottom - layout.track.bottom == 8.0f * scale,
                  "narrow and scaled drive rows separate title, precision caption and capacity bar");
        }
    }
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
