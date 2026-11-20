#pragma once

#include "../fs/bounded_enumeration.h"
#include "../fs/enumeration_schedule.h"
#include <utility>

namespace pulse {

// Filesystem providers own their inputs. The UI consumes the result only after
// publication and can release the mailbox without waiting for a stalled share.
template<class Value>
struct AddressLookup {
    std::atomic_bool cancelled{false};
    std::atomic_bool ready{false};
    std::optional<Value> result;
};

template<class Value, class Provider>
void StartAddressLookup(const std::shared_ptr<AddressLookup<Value>>& lookup, bool network,
                        Provider provider, HWND owner = nullptr, UINT ready_message = 0) {
    try {
        std::thread([lookup, network, provider = std::move(provider), owner, ready_message]() mutable {
            try {
                lookup->result.emplace(fs::RunBoundedEnumeration<Value>(network,
                    [provider = std::move(provider)](const std::atomic_bool& cancelled) mutable {
                        SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr);
                        return provider(cancelled);
                    },
                    [&] { return lookup->cancelled.load(); }, std::chrono::seconds(5)));
            } catch (...) {
                // Cancellation, expiry and provider failure publish an empty result.
            }
            lookup->ready.store(true);
            if (owner && !lookup->cancelled) PostMessageW(owner, ready_message, 0, 0);
        }).detach();
    } catch (...) {
        lookup->ready.store(true);
        if (owner) PostMessageW(owner, ready_message, 0, 0);
    }
}

} // namespace pulse
