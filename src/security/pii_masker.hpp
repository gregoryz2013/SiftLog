#pragma once

#include <string>
#include <string_view>
#include "storage/log_entry.hpp"

// Linear-time O(N), stack-safe PII & secret redactor (immune to regex backtracking / ReDoS)
class PiiMasker {
public:
    explicit PiiMasker(bool enabled = true);

    bool is_enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

    std::string mask_string(std::string_view input) const;
    void mask_entry(LogEntry& entry) const;

private:
    bool enabled_;
};
