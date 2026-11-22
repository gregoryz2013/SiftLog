#pragma once

#include <string>
#include <chrono>
#include <nlohmann/json.hpp>

struct EnterpriseLicense {
    bool valid = false;
    std::string customer;
    std::string plan = "community";
    int64_t expires_ms = 0;
    std::string raw_token;
    std::string error;

    bool is_active() const noexcept {
        if (!valid) return false;
        if (expires_ms <= 0) return true;
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return now < expires_ms;
    }
};

class LicenseManager {
public:
    static EnterpriseLicense parse_license_file(const std::string& path);
    static EnterpriseLicense parse_license_token(const std::string& token);
};
