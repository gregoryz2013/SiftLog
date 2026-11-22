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
        // NEVER deduplicate errors or warnings
        if (entry.level == "ERROR" || entry.level == "FATAL" || entry.level == "WARN") {
            result.push_back(std::move(entry));
            continue;
        }

        // Check if identical to the immediately preceding entry in this output batch
        if (!result.empty() &&
            result.back().service == entry.service &&
            result.back().level == entry.level &&
            result.back().message == entry.message &&
            std::abs(entry.timestamp - result.back().timestamp) <= window_ms) {
            result.back().repeat_count += entry.repeat_count;
            continue;
        }

        // Check against stateful service cache
        auto& cached = service_recent_[entry.service];
        if (cached.message == entry.message &&
            (entry.timestamp - cached.last_seen_ms) <= window_ms &&
            !result.empty()) {
            result.back().repeat_count += entry.repeat_count;
            cached.last_seen_ms = entry.timestamp;
            continue;
        }

        cached.message = entry.message;
        cached.last_seen_ms = entry.timestamp;
        result.push_back(std::move(entry));
    }

    return result;
}
