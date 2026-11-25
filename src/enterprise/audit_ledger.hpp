#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

struct AuditRecord {
    int64_t timestamp = 0;
    std::string actor;      // user or client IP
    std::string action;     // "SEARCH", "INGEST", "AUTH_LOGIN", "CONFIG"
    std::string details;
    std::string status;     // "SUCCESS", "DENIED"
    std::string prev_hash;
    std::string hash;

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"timestamp", timestamp},
            {"actor", actor},
            {"action", action},
            {"details", details},
            {"status", status},
            {"prev_hash", prev_hash},
            {"hash", hash}
        };
    }
};

class AuditLedger {
public:
    explicit AuditLedger(std::string log_file_path);

    void record_event(const std::string& actor,
                      const std::string& action,
                      const std::string& details,
                      const std::string& status = "SUCCESS");

    std::vector<AuditRecord> get_recent_events(size_t limit = 50);

private:
    std::string log_file_path_;
    std::string last_hash_;
    std::mutex mutex_;
    std::vector<AuditRecord> memory_cache_;
};
