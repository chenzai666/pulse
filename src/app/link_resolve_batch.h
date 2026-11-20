#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pulse::app {

// A stalled provider may outlive the caller. Keep its quota separate from
// directory reads so optional link metadata cannot starve navigation.
inline std::atomic<unsigned> active_link_batches{0};

template<class Entry, class Select, class Resolve, class Cancelled>
void ResolveLinkBatch(std::vector<Entry>& entries, Select select, Resolve resolve, Cancelled cancelled,
                      std::chrono::milliseconds budget = std::chrono::milliseconds(100)) {
    if (entries.empty() || cancelled()) return;
    unsigned active = active_link_batches.load();
    do {
        if (active >= 2) return;
    } while (!active_link_batches.compare_exchange_weak(active, active + 1));

    struct Candidate {
        size_t index;
        Entry entry;
    };
    struct State {
        std::atomic_bool stop{false};
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<Candidate> candidates;
        size_t completed = 0;
        bool done = false;
    };
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::shared_ptr<State> state;
    std::thread worker;
    try {
        state = std::make_shared<State>();
        for (size_t i = 0; i < entries.size(); ++i) {
            if ((i & 127u) == 0 &&
                (cancelled() || std::chrono::steady_clock::now() >= deadline)) break;
            if (select(entries[i])) state->candidates.push_back({i, entries[i]});
        }
        if (state->candidates.empty() || cancelled() || std::chrono::steady_clock::now() >= deadline) {
            --active_link_batches;
            return;
        }
        worker = std::thread([state, deadline, resolve = std::move(resolve)]() mutable {
            try {
                for (auto& candidate : state->candidates) {
                    if (state->stop || std::chrono::steady_clock::now() >= deadline) break;
                    Entry entry = std::move(candidate.entry);
                    resolve(entry);
                    std::lock_guard lock(state->mutex);
                    if (state->stop || std::chrono::steady_clock::now() >= deadline) break;
                    candidate.entry = std::move(entry);
                    ++state->completed;
                }
            } catch (...) {
                // The listing remains usable with the metadata completed so far.
            }
            {
                std::lock_guard lock(state->mutex);
                state->done = true;
            }
            --active_link_batches;
            state->ready.notify_one();
        });
    } catch (...) {
        --active_link_batches;
        return;
    }

    bool done = false;
    try {
        std::unique_lock lock(state->mutex);
        while (!state->done && !cancelled() && std::chrono::steady_clock::now() < deadline)
            state->ready.wait_until(lock, (std::min)(deadline,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
        state->stop = true;
        if (!cancelled()) {
            for (size_t i = 0; i < state->completed; ++i) {
                auto& candidate = state->candidates[i];
                entries[candidate.index] = std::move(candidate.entry);
            }
        }
        done = state->done;
    } catch (...) {
        state->stop = true;
    }
    // Provider inputs outlive a timeout. A late result cannot mutate entries.
    if (done) worker.join();
    else worker.detach();
}

} // namespace pulse::app
