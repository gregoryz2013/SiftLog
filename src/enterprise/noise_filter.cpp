#include "enterprise/noise_filter.hpp"

NoiseFilter::NoiseFilter(bool enabled, int window_seconds)
    : enabled_(enabled), window_seconds_(window_seconds) {}

std::vector<LogEntry> NoiseFilter::dedup_batch(std::vector<LogEntry> entries) {
    if (!enabled_ || entries.empty()) {
        return entries;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LogEntry> result;
    result.reserve(entries.size());

    int64_t window_ms = static_cast<int64_t>(window_seconds_) * 1000LL;

    for (auto& entry : entries) {
        if (entry.repeat_count == 0) entry.repeat_count = 1;

        // Check if an identical entry exists within recent output window
        bool deduplicated = false;
        for (auto it = result.rbegin(); it != result.rend(); ++it) {
            if (it->service == entry.service &&
                it->level == entry.level &&
                it->message == entry.message &&
                std::abs(entry.timestamp - it->timestamp) <= window_ms) {
                it->repeat_count += entry.repeat_count;
                saved_events_.fetch_add(entry.repeat_count, std::memory_order_relaxed);
                deduplicated = true;
                break;
            }
        }

        if (deduplicated) {
            continue;
        }

        result.push_back(std::move(entry));
    }

    return result;
}
