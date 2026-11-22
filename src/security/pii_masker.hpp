#pragma once

#include <string>
#include <string_view>
#include <regex>
#include "storage/log_entry.hpp"

class PiiMasker {
public:
    explicit PiiMasker(bool enabled = true);

    bool is_enabled() const noexcept { return enabled_; }
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

    std::string mask_string(const std::string& input) const;
    void mask_entry(LogEntry& entry) const;

private:
    bool enabled_;
    std::regex token_regex_;
    std::regex jwt_regex_;
    std::regex secret_regex_;
    std::regex card_regex_;
    std::regex passport_regex_;
    std::regex snils_regex_;
};
