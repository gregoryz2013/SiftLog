#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "../storage/log_entry.hpp"

class NoiseFilter {
public:
    explicit NoiseFilter(bool enabled = true, int window_seconds = 5);

    bool is_enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

    // Deduplicates a batch of incoming logs, aggregating consecutive identical INFO/DEBUG entries
    std::vector<LogEntry> dedup_batch(std::vector<LogEntry> entries);

private:
    bool enabled_;
    int window_seconds_;
    std::mutex mutex_;
    struct RecentSeen {
        std::string message;
        int64_t last_seen_ms = 0;
        size_t count = 1;
    };
    std::unordered_map<std::string, RecentSeen> service_recent_;
};
