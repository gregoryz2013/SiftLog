#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>
#include "../storage/log_entry.hpp"

struct IncidentAnalysis {
    bool has_incident = false;
    std::string status = "RESOLVED"; // "ACTIVE" or "RESOLVED"
    int64_t incident_start_ms = 0;
    int64_t incident_end_ms = 0;
    
    // Root Cause
    std::string root_service;
    std::string root_error;
    int64_t root_timestamp_ms = 0;
    std::string root_trace_id;

    // Cascade metrics
    size_t total_errors = 0;
    std::unordered_map<std::string, size_t> errors_by_service;
    std::vector<std::string> impacted_services;

    nlohmann::json to_json() const;
};

class RcaEngine {
public:
    explicit RcaEngine(int window_seconds = 60, size_t error_threshold = 3);

    // Observe incoming logs to track real-time incident states
    void observe(const std::vector<LogEntry>& entries);

    // Get current incident status and root-cause analysis
    IncidentAnalysis get_current_incident() const;

    // Perform retrospective RCA on a provided list of logs
    static IncidentAnalysis analyze_window(const std::vector<LogEntry>& logs);

private:
    int window_seconds_;
    size_t error_threshold_;
    mutable std::mutex mutex_;

    struct ErrorEvent {
        int64_t timestamp_ms;
        std::string service;
        std::string message;
        std::string trace_id;
    };

    std::vector<ErrorEvent> recent_errors_;
    int64_t last_error_ms_ = 0;
};
