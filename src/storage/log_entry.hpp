#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <cmath>
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

        // 1. Timestamp extraction (supports ms, s, float seconds, ISO strings)
        if (j.contains("timestamp")) {
            if (j["timestamp"].is_number_integer()) {
                entry.timestamp = j["timestamp"].get<int64_t>();
                // If timestamp looks like seconds (< 10000000000), convert to ms
                if (entry.timestamp > 0 && entry.timestamp < 10000000000LL) {
                    entry.timestamp *= 1000LL;
                }
            } else if (j["timestamp"].is_number_float()) {
                entry.timestamp = static_cast<int64_t>(j["timestamp"].get<double>() * 1000.0);
            } else if (j["timestamp"].is_string()) {
                try {
                    entry.timestamp = std::stoll(j["timestamp"].get<std::string>());
                    if (entry.timestamp > 0 && entry.timestamp < 10000000000LL) {
                        entry.timestamp *= 1000LL;
                    }
                } catch (...) {
                    entry.timestamp = now_ms();
                }
            }
        } else if (j.contains("time")) {
            if (j["time"].is_number_integer()) {
                entry.timestamp = j["time"].get<int64_t>();
                if (entry.timestamp > 0 && entry.timestamp < 10000000000LL) {
                    entry.timestamp *= 1000LL;
                }
            } else if (j["time"].is_number_float()) {
                entry.timestamp = static_cast<int64_t>(j["time"].get<double>() * 1000.0);
            }
        } else if (j.contains("date")) { // Fluent Bit date field
            if (j["date"].is_number_float()) {
                entry.timestamp = static_cast<int64_t>(j["date"].get<double>() * 1000.0);
            } else if (j["date"].is_number_integer()) {
                entry.timestamp = j["date"].get<int64_t>();
                if (entry.timestamp > 0 && entry.timestamp < 10000000000LL) {
                    entry.timestamp *= 1000LL;
                }
            }
        } else if (j.contains("@timestamp") && j["@timestamp"].is_number_integer()) {
            entry.timestamp = j["@timestamp"].get<int64_t>();
        }

        if (entry.timestamp <= 0) {
            entry.timestamp = now_ms();
        }

        // 2. Level extraction
        if (j.contains("level") && j["level"].is_string()) {
            entry.level = normalize_level(j["level"].get<std::string>());
        } else if (j.contains("lvl") && j["lvl"].is_string()) {
            entry.level = normalize_level(j["lvl"].get<std::string>());
        } else if (j.contains("severity") && j["severity"].is_string()) {
            entry.level = normalize_level(j["severity"].get<std::string>());
        } else if (j.contains("stream") && j["stream"].is_string()) {
            // Docker / k8s stream detection
            std::string st = j["stream"].get<std::string>();
            entry.level = (st == "stderr") ? "ERROR" : "INFO";
        }

        // 3. Service / Application extraction (Kubernetes, Docker, Vector, Fluent Bit)
        if (j.contains("service") && j["service"].is_string()) {
            entry.service = j["service"].get<std::string>();
        } else if (j.contains("app") && j["app"].is_string()) {
            entry.service = j["app"].get<std::string>();
        } else if (j.contains("kubernetes") && j["kubernetes"].is_object()) {
            const auto& k = j["kubernetes"];
            if (k.contains("container_name") && k["container_name"].is_string()) {
                entry.service = k["container_name"].get<std::string>();
            } else if (k.contains("pod_name") && k["pod_name"].is_string()) {
                entry.service = k["pod_name"].get<std::string>();
            }
        } else if (j.contains("container_name") && j["container_name"].is_string()) {
            entry.service = j["container_name"].get<std::string>();
        } else if (j.contains("logger") && j["logger"].is_string()) {
            entry.service = j["logger"].get<std::string>();
        } else if (j.contains("tag") && j["tag"].is_string()) {
            entry.service = j["tag"].get<std::string>();
        }

        // 4. Message extraction
        if (j.contains("message") && j["message"].is_string()) {
            entry.message = j["message"].get<std::string>();
        } else if (j.contains("log") && j["log"].is_string()) { // Docker / Fluent Bit log line
            entry.message = j["log"].get<std::string>();
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
