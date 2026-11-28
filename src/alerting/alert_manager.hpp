#pragma once

#include "../storage/log_entry.hpp"
#include <string>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <cstdint>
#include <chrono>

struct AlertEvent {
    std::string service;
    size_t error_count = 0;
    int64_t timestamp = 0;
    std::string time_str;
    std::string sample_error;

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"service", service},
            {"error_count", error_count},
            {"timestamp", timestamp},
            {"time", time_str},
            {"sample_error", sample_error}
        };
    }
};

class AlertManager {
public:
    AlertManager(std::string tg_token,
                 std::string tg_chat_id,
                 size_t error_threshold = 10,
                 int window_seconds = 60,
                 int cooldown_seconds = 60,
                 std::string webhook_url = "");

    void record_log(const LogEntry& log);
    std::vector<AlertEvent> get_recent_alerts() const;

private:
    void trigger_alert(const std::string& service, size_t error_count, const std::string& sample_error);
    void send_telegram_alert(const std::string& service, size_t count, const std::string& sample_error);
    void send_webhook_alert(const std::string& service, size_t count, const std::string& sample_error);

    std::string tg_token_;
    std::string tg_chat_id_;
    size_t error_threshold_;
    int window_seconds_;
    int cooldown_seconds_;
    std::string webhook_url_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::deque<int64_t>> service_error_timestamps_;
    std::unordered_map<std::string, int64_t> last_alert_time_;
    std::vector<AlertEvent> recent_alerts_;
};
