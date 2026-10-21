#pragma once

#include <cstdint>
#include <map>

namespace pulse::app {

class ExplorerScanSchedule {
public:
    static constexpr uint64_t kIntervalMs = 500;
    bool Due(uint64_t now, bool pending) const {
        return pending && (!scanned_ || now - last_scan_ >= kIntervalMs);
    }
    void Scanned(uint64_t now) { last_scan_ = now; scanned_ = true; }

private:
    uint64_t last_scan_ = 0;
    bool scanned_ = false;
};

struct ExplorerWindowIdentity {
    uintptr_t window = 0;
    uint32_t process_id = 0;
    uint32_t thread_id = 0;
    uint64_t process_created = 0;
    bool operator==(const ExplorerWindowIdentity&) const = default;
};

class ExplorerTakeoverGuard {
public:
    static constexpr unsigned kMaxReassertions = 3;

    uint64_t Track(const ExplorerWindowIdentity& identity) {
        if (!identity.window || !identity.process_id || !identity.thread_id || !identity.process_created) return 0;
        auto [it, inserted] = windows_.try_emplace(identity.window);
        if (inserted || it->second.identity != identity)
            it->second = Entry{identity, ++next_token_, false, 0};
        return it->second.token;
    }

    bool IsCurrent(const ExplorerWindowIdentity& identity, uint64_t token) const {
        const auto it = windows_.find(identity.window);
        return token && it != windows_.end() && it->second.token == token && it->second.identity == identity;
    }

    bool IsReleased(const ExplorerWindowIdentity& identity, uint64_t token) const {
        const auto it = windows_.find(identity.window);
        return IsCurrent(identity, token) && it->second.released;
    }

    bool Release(const ExplorerWindowIdentity& identity, uint64_t token) {
        if (!IsCurrent(identity, token)) return false;
        windows_.find(identity.window)->second.released = true;
        return true;
    }

    bool AllowReassert(const ExplorerWindowIdentity& identity, uint64_t token) {
        if (!IsCurrent(identity, token)) return false;
        auto& entry = windows_.find(identity.window)->second;
        if (entry.released || entry.reassertions >= kMaxReassertions) return false;
        ++entry.reassertions;
        return true;
    }

    void Forget(uintptr_t window) { windows_.erase(window); }

private:
    struct Entry {
        ExplorerWindowIdentity identity;
        uint64_t token = 0;
        bool released = false;
        unsigned reassertions = 0;
    };
    std::map<uintptr_t, Entry> windows_;
    uint64_t next_token_ = 0;
};

} // namespace pulse::app
