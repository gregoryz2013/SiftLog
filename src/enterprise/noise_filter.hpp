#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "../storage/log_entry.hpp"

class NoiseFilter {
public:
    explicit NoiseFilter(bool enabled = true, int window_seconds = 5);

    bool is_enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

    // Deduplicates a batch of incoming logs, aggregating consecutive identical entries
    std::vector<LogEntry> dedup_batch(std::vector<LogEntry> entries);

    size_t get_saved_events() const noexcept { return saved_events_.load(std::memory_order_relaxed); }

private:
    bool enabled_;
    int window_seconds_;
    std::mutex mutex_;
    std::atomic<size_t> saved_events_{0};
    struct RecentSeen {
        std::string message;
        int64_t last_seen_ms = 0;
        size_t count = 1;
    };
    std::unordered_map<std::string, RecentSeen> service_recent_;
};
