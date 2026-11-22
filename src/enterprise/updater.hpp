#pragma once

#include <string>
#include <nlohmann/json.hpp>

struct UpdateCheckResult {
    bool update_available = false;
    std::string current_version;
    std::string latest_version;
    std::string download_url;
    std::string release_notes;
    std::string error;

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"update_available", update_available},
            {"current_version", current_version},
            {"latest_version", latest_version},
            {"download_url", download_url},
            {"release_notes", release_notes},
            {"error", error}
        };
    }
};

class EnterpriseUpdater {
public:
    explicit EnterpriseUpdater(std::string current_version, std::string update_server_url = "");

    UpdateCheckResult check_update(const std::string& license_token = "");
    bool apply_update(const std::string& download_url, std::string& error_out);

    const std::string& current_version() const noexcept { return current_version_; }

private:
    std::string current_version_;
    std::string update_server_url_;
};
