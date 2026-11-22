#include "api_server.hpp"
#include "../web_assets.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

ApiServer::ApiServer(std::shared_ptr<LogStorage> storage,
                     std::shared_ptr<AlertManager> alert_manager,
                     std::string host,
                     int port)
    : storage_(std::move(storage)),
      alert_manager_(std::move(alert_manager)),
      host_(std::move(host)),
      port_(port),
      start_time_(std::chrono::steady_clock::now()) {
    setup_routes();
}

std::string ApiServer::get_client_ip(const httplib::Request& req) {
    if (req.has_header("X-Forwarded-For")) {
        std::string xff = req.get_header_value("X-Forwarded-For");
        size_t comma = xff.find(',');
        return (comma != std::string::npos) ? xff.substr(0, comma) : xff;
    }
    if (req.has_header("X-Real-IP")) {
        return req.get_header_value("X-Real-IP");
    }
    return req.remote_addr;
}

void ApiServer::setup_routes() {
    // Limit request payload to 50MB to prevent memory exhaustion / DoS attacks
    svr_.set_payload_max_length(50 * 1024 * 1024);

    svr_.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
        res.status = 204;
    });

    // Health check for load balancers and reverse proxies (nginx upstream, apache proxy_balancer)
    svr_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");
        res.set_content(R"({"status":"ok"})", "application/json");
    });
    svr_.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    // Embedded Web Dashboard
    svr_.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_content(INDEX_HTML.data(), INDEX_HTML.size(), "text/html; charset=utf-8");
    });

    // Ingestion API
    svr_.Post("/api/v1/logs", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        if (req.body.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"empty request body"})", "application/json");
            return;
        }

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            nlohmann::json err = {{"error", "invalid json"}, {"detail", e.what()}};
            res.set_content(err.dump(), "application/json");
            return;
        }

        std::vector<LogEntry> entries;
        if (j.is_array()) {
            entries.reserve(j.size());
            for (const auto& item : j) {
                if (item.is_object()) {
                    LogEntry entry = LogEntry::from_json(item);
                    alert_manager_->record_log(entry);
                    entries.push_back(std::move(entry));
                }
            }
        } else if (j.is_object()) {
            if (j.contains("logs") && j["logs"].is_array()) {
                entries.reserve(j["logs"].size());
                for (const auto& item : j["logs"]) {
                    if (item.is_object()) {
                        LogEntry entry = LogEntry::from_json(item);
                        alert_manager_->record_log(entry);
                        entries.push_back(std::move(entry));
                    }
                }
            } else {
                LogEntry entry = LogEntry::from_json(j);
                alert_manager_->record_log(entry);
                entries.push_back(std::move(entry));
            }
        } else {
            res.status = 400;
            res.set_content(R"({"error":"expected object or array"})", "application/json");
            return;
        }

        size_t count = entries.size();
        storage_->append_batch(entries);

        nlohmann::json response = {
            {"status", "ok"},
            {"ingested", count}
        };
        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });

    // Search API
    svr_.Get("/api/v1/search", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        SearchParams params;
        if (req.has_param("q")) {
            params.query = req.get_param_value("q");
        }
        if (req.has_param("service")) {
            params.service = req.get_param_value("service");
        }
        if (req.has_param("level")) {
            params.level = LogEntry::normalize_level(req.get_param_value("level"));
        }
        if (req.has_param("from")) {
            try {
                params.from_ts = std::stoll(req.get_param_value("from"));
            } catch (...) {}
        }
        if (req.has_param("to")) {
            try {
                params.to_ts = std::stoll(req.get_param_value("to"));
            } catch (...) {}
        }
        if (req.has_param("limit")) {
            try {
                size_t l = std::stoull(req.get_param_value("limit"));
                params.limit = std::clamp<size_t>(l, 1, 1000);
            } catch (...) {}
        }

        SearchResult search_result = storage_->search(params);

        nlohmann::json logs_json = nlohmann::json::array();
        for (const auto& log : search_result.logs) {
            logs_json.push_back(log.to_json());
        }

        nlohmann::json response = {
            {"total_scanned_chunks", search_result.total_scanned_chunks},
            {"matched_chunks", search_result.matched_chunks},
            {"total_matched", search_result.total_matched},
            {"duration_ms", search_result.duration_ms},
            {"logs", logs_json}
        };

        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });

    // Server Stats API
    svr_.Get("/api/v1/stats", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        auto now = std::chrono::steady_clock::now();
        int64_t uptime_sec = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

        StorageStats stats = storage_->get_stats();

        double ratio = 1.0;
        if (stats.disk_bytes_compressed > 0) {
            ratio = static_cast<double>(stats.disk_bytes_uncompressed) /
                    static_cast<double>(stats.disk_bytes_compressed);
        }

        std::ostringstream ratio_ss;
        ratio_ss << std::fixed << std::setprecision(1) << ratio << "x";

        nlohmann::json alerts_json = nlohmann::json::array();
        for (const auto& alert : alert_manager_->get_recent_alerts()) {
            alerts_json.push_back(alert.to_json());
        }

        nlohmann::json response = {
            {"uptime_seconds", uptime_sec},
            {"total_logs_ingested", stats.total_logs_ingested},
            {"total_chunks_on_disk", stats.total_chunks_on_disk},
            {"disk_bytes_compressed", stats.disk_bytes_compressed},
            {"disk_bytes_uncompressed", stats.disk_bytes_uncompressed},
            {"compression_ratio", ratio_ss.str()},
            {"services", stats.known_services},
            {"recent_alerts", alerts_json}
        };

        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });
}

void ApiServer::start() {
    std::cout << "[server] listening on " << host_ << ":" << port_ << std::endl;
    svr_.listen(host_, port_);
}

void ApiServer::stop() {
    svr_.stop();
}
