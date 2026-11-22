#include "api_server.hpp"
#include "../web_assets.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

ApiServer::ApiServer(std::shared_ptr<LogStorage> storage,
                     std::shared_ptr<AlertManager> alert_manager,
                     std::string host,
                     int port,
                     std::string auth_user,
                     std::string auth_password)
    : storage_(std::move(storage)),
      alert_manager_(std::move(alert_manager)),
      host_(std::move(host)),
      port_(port),
      auth_user_(std::move(auth_user)),
      auth_password_(std::move(auth_password)),
      start_time_(std::chrono::steady_clock::now()) {
    setup_routes();
}

std::string ApiServer::base64_decode(std::string_view in) {
    static const std::string b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) {
        T[static_cast<unsigned char>(b64[i])] = i;
    }

    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

bool ApiServer::check_auth(const httplib::Request& req, httplib::Response& res) {
    if (auth_user_.empty()) return true;

    if (req.has_header("Authorization")) {
        std::string auth = req.get_header_value("Authorization");
        if (auth.rfind("Basic ", 0) == 0) {
            std::string decoded = base64_decode(auth.substr(6));
            auto colon = decoded.find(':');
            if (colon != std::string::npos) {
                std::string u = decoded.substr(0, colon);
                std::string p = decoded.substr(colon + 1);
                if (u == auth_user_ && p == auth_password_) {
                    return true;
                }
            }
        }
    }

    res.status = 401;
    res.set_header("WWW-Authenticate", "Basic realm=\"SiftLog\"");
    res.set_content(R"({"error":"unauthorized"})", "application/json");
    return false;
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
    svr_.set_payload_max_length(50 * 1024 * 1024);

    svr_.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
        res.status = 204;
    });

    // Health checks (always public, no auth required)
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

    // Web Dashboard
    svr_.Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Cache-Control", "no-cache");
        res.set_content(INDEX_HTML.data(), INDEX_HTML.size(), "text/html; charset=utf-8");
    });

    // Ingestion API (JSON array, single object, or NDJSON from Vector/FluentBit)
    svr_.Post("/api/v1/logs", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        if (req.body.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"empty request body"})", "application/json");
            return;
        }

        std::vector<LogEntry> entries;
        bool parsed_json = false;

        // Try standard JSON parse
        try {
            nlohmann::json j = nlohmann::json::parse(req.body);
            parsed_json = true;
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
            }
        } catch (...) {
            parsed_json = false;
        }

        // Fallback: parse NDJSON (Newline-Delimited JSON)
        if (!parsed_json) {
            std::string_view sv(req.body);
            size_t start = 0;
            while (start < sv.size()) {
                size_t end = sv.find('\n', start);
                if (end == std::string_view::npos) end = sv.size();
                std::string_view line = sv.substr(start, end - start);
                start = end + 1;
                if (line.empty()) continue;

                try {
                    nlohmann::json item = nlohmann::json::parse(line);
                    LogEntry entry = LogEntry::from_json(item);
                    alert_manager_->record_log(entry);
                    entries.push_back(std::move(entry));
                } catch (...) {}
            }
        }

        if (entries.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"could not parse valid json logs"})", "application/json");
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

    // Loki-compatible push API (Standard for Fluent Bit out_loki, Vector loki sink, Promtail)
    svr_.Post("/loki/api/v1/push", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content("Invalid Loki JSON", "text/plain");
            return;
        }

        std::vector<LogEntry> entries;
        if (j.contains("streams") && j["streams"].is_array()) {
            for (const auto& stream_obj : j["streams"]) {
                std::string stream_service = "default";
                std::string stream_level = "INFO";

                if (stream_obj.contains("stream") && stream_obj["stream"].is_object()) {
                    const auto& st = stream_obj["stream"];
                    if (st.contains("service") && st["service"].is_string()) stream_service = st["service"];
                    else if (st.contains("app") && st["app"].is_string()) stream_service = st["app"];
                    else if (st.contains("job") && st["job"].is_string()) stream_service = st["job"];
                    else if (st.contains("container") && st["container"].is_string()) stream_service = st["container"];

                    if (st.contains("level") && st["level"].is_string()) stream_level = LogEntry::normalize_level(st["level"]);
                }

                if (stream_obj.contains("values") && stream_obj["values"].is_array()) {
                    for (const auto& val_pair : stream_obj["values"]) {
                        if (val_pair.is_array() && val_pair.size() >= 2) {
                            int64_t ts_ms = LogEntry::now_ms();
                            std::string ts_str = val_pair[0].is_string() ? val_pair[0].get<std::string>() : val_pair[0].dump();
                            try {
                                int64_t ns = std::stoll(ts_str);
                                ts_ms = ns / 1000000LL;
                            } catch (...) {}

                            std::string line = val_pair[1].is_string() ? val_pair[1].get<std::string>() : val_pair[1].dump();

                            LogEntry entry;
                            entry.timestamp = ts_ms;
                            entry.service = stream_service;
                            entry.level = stream_level;

                            // Check if line itself is JSON
                            try {
                                nlohmann::json line_j = nlohmann::json::parse(line);
                                entry = LogEntry::from_json(line_j);
                                if (entry.service == "default") entry.service = stream_service;
                                if (entry.timestamp <= 0) entry.timestamp = ts_ms;
                            } catch (...) {
                                entry.message = line;
                                entry.raw_json = nlohmann::json{
                                    {"timestamp", ts_ms},
                                    {"service", stream_service},
                                    {"level", stream_level},
                                    {"message", line}
                                }.dump();
                            }

                            alert_manager_->record_log(entry);
                            entries.push_back(std::move(entry));
                        }
                    }
                }
            }
        }

        if (!entries.empty()) {
            storage_->append_batch(entries);
        }

        // Standard Loki push response is 204 No Content
        res.status = 204;
    });

    // Search API
    svr_.Get("/api/v1/search", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
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
    svr_.Get("/api/v1/stats", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
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
    if (!auth_user_.empty()) {
        std::cout << "[server] basic auth: enabled (user: " << auth_user_ << ")" << std::endl;
    }
    svr_.listen(host_, port_);
}

void ApiServer::stop() {
    svr_.stop();
}
