#include "enterprise/rca_engine.hpp"
#include <algorithm>

nlohmann::json IncidentAnalysis::to_json() const {
    nlohmann::json j;
    j["has_incident"] = has_incident;
    j["status"] = status;
    j["incident_start_ms"] = incident_start_ms;
    j["incident_end_ms"] = incident_end_ms;
    
    if (has_incident) {
        j["root_cause"] = {
            {"service", root_service},
            {"error", root_error},
            {"timestamp_ms", root_timestamp_ms},
            {"trace_id", root_trace_id}
        };
        j["cascade"] = {
            {"total_errors", total_errors},
            {"impacted_services", impacted_services},
            {"errors_by_service", errors_by_service}
        };
    } else {
        j["root_cause"] = nullptr;
        j["cascade"] = nullptr;
    }
    return j;
}

RcaEngine::RcaEngine(int window_seconds, size_t error_threshold)
    : window_seconds_(window_seconds), error_threshold_(error_threshold) {}

void RcaEngine::observe(const std::vector<LogEntry>& entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now_ms = LogEntry::now_ms();
    int64_t window_ms = static_cast<int64_t>(window_seconds_) * 1000LL;

    for (const auto& entry : entries) {
        if (entry.level == "ERROR" || entry.level == "FATAL") {
            recent_errors_.push_back({
                entry.timestamp,
                entry.service,
                entry.message,
                entry.trace_id
            });
            if (entry.timestamp > last_error_ms_) {
                last_error_ms_ = entry.timestamp;
            }
        }
    }

    // Prune errors older than window from current time
    int64_t cutoff = now_ms - window_ms;
    recent_errors_.erase(
        std::remove_if(recent_errors_.begin(), recent_errors_.end(),
                       [cutoff](const ErrorEvent& ev) { return ev.timestamp_ms < cutoff; }),
        recent_errors_.end());
}

IncidentAnalysis RcaEngine::get_current_incident() const {
    std::lock_guard<std::mutex> lock(mutex_);
    IncidentAnalysis analysis;

    if (recent_errors_.size() < error_threshold_) {
        analysis.has_incident = false;
        analysis.status = "RESOLVED";
        return analysis;
    }

    // Sort chronologically to identify first root cause
    std::vector<ErrorEvent> sorted_errors = recent_errors_;
    std::sort(sorted_errors.begin(), sorted_errors.end(),
              [](const ErrorEvent& a, const ErrorEvent& b) {
                  return a.timestamp_ms < b.timestamp_ms;
              });

    analysis.has_incident = true;
    int64_t now_ms = LogEntry::now_ms();
    int64_t silence_threshold_ms = 30000; // 30 sec without errors considered resolving
    analysis.status = (now_ms - last_error_ms_ > silence_threshold_ms) ? "RESOLVED" : "ACTIVE";

    analysis.incident_start_ms = sorted_errors.front().timestamp_ms;
    analysis.incident_end_ms = sorted_errors.back().timestamp_ms;

    // Root cause is the earliest failure
    const auto& root = sorted_errors.front();
    analysis.root_service = root.service;
    analysis.root_error = root.message;
    analysis.root_timestamp_ms = root.timestamp_ms;
    analysis.root_trace_id = root.trace_id;

    analysis.total_errors = sorted_errors.size();
    for (const auto& ev : sorted_errors) {
        analysis.errors_by_service[ev.service]++;
    }

    for (const auto& [svc, _] : analysis.errors_by_service) {
        if (svc != analysis.root_service) {
            analysis.impacted_services.push_back(svc);
        }
    }

    return analysis;
}

IncidentAnalysis RcaEngine::analyze_window(const std::vector<LogEntry>& logs) {
    IncidentAnalysis analysis;
    std::vector<const LogEntry*> errors;
    for (const auto& l : logs) {
        if (l.level == "ERROR" || l.level == "FATAL") {
            errors.push_back(&l);
        }
    }

    if (errors.size() < 2) {
        analysis.has_incident = false;
        return analysis;
    }

    std::sort(errors.begin(), errors.end(), [](const LogEntry* a, const LogEntry* b) {
        return a->timestamp < b->timestamp;
    });

    analysis.has_incident = true;
    analysis.status = "ACTIVE";
    analysis.incident_start_ms = errors.front()->timestamp;
    analysis.incident_end_ms = errors.back()->timestamp;

    const auto* root = errors.front();
    analysis.root_service = root->service;
    analysis.root_error = root->message;
    analysis.root_timestamp_ms = root->timestamp;
    analysis.root_trace_id = root->trace_id;
    analysis.total_errors = errors.size();

    for (const auto* e : errors) {
        analysis.errors_by_service[e->service]++;
    }
    for (const auto& [svc, _] : analysis.errors_by_service) {
        if (svc != analysis.root_service) {
            analysis.impacted_services.push_back(svc);
        }
    }

    return analysis;
}
