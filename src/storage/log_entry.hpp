#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <nlohmann/json.hpp>

struct LogEntry {
    int64_t timestamp = 0;
    std::string level = "INFO";
    std::string service = "default";
    std::string message;
    std::string raw_json;

    static int64_t now_ms() noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static std::string normalize_level(std::string lvl) {
        std::transform(lvl.begin(), lvl.end(), lvl.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return lvl.empty() ? "INFO" : lvl;
    }

    static LogEntry from_json(const nlohmann::json& j) {
        LogEntry entry;

        if (j.contains("timestamp") && j["timestamp"].is_number()) {
            entry.timestamp = j["timestamp"].get<int64_t>();
        } else if (j.contains("time") && j["time"].is_number()) {
            entry.timestamp = j["time"].get<int64_t>();
        } else if (j.contains("@timestamp") && j["@timestamp"].is_number()) {
            entry.timestamp = j["@timestamp"].get<int64_t>();
        } else if (j.contains("timestamp") && j["timestamp"].is_string()) {
            try {
                entry.timestamp = std::stoll(j["timestamp"].get<std::string>());
            } catch (...) {
                entry.timestamp = now_ms();
            }
        } else {
            entry.timestamp = now_ms();
        }

        if (j.contains("level") && j["level"].is_string()) {
            entry.level = normalize_level(j["level"].get<std::string>());
        } else if (j.contains("lvl") && j["lvl"].is_string()) {
            entry.level = normalize_level(j["lvl"].get<std::string>());
        } else if (j.contains("severity") && j["severity"].is_string()) {
            entry.level = normalize_level(j["severity"].get<std::string>());
        }

        if (j.contains("service") && j["service"].is_string()) {
            entry.service = j["service"].get<std::string>();
        } else if (j.contains("app") && j["app"].is_string()) {
            entry.service = j["app"].get<std::string>();
        } else if (j.contains("logger") && j["logger"].is_string()) {
            entry.service = j["logger"].get<std::string>();
        }

        if (j.contains("message") && j["message"].is_string()) {
            entry.message = j["message"].get<std::string>();
        } else if (j.contains("msg") && j["msg"].is_string()) {
            entry.message = j["msg"].get<std::string>();
        } else if (j.contains("error") && j["error"].is_string()) {
            entry.message = j["error"].get<std::string>();
        } else {
            entry.message = j.dump();
        }

        entry.raw_json = j.dump();
        return entry;
    }

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"timestamp", timestamp},
            {"level", level},
            {"service", service},
            {"message", message},
            {"raw_json", raw_json}
        };
    }
};
