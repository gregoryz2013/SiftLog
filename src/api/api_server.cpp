#include "api_server.hpp"
#include "../web_assets.hpp"
#include "../utils/base64.hpp"
#include <iostream>
#include <iomanip>
#include <map>
#include <sstream>

ApiServer::ApiServer(std::shared_ptr<LogStorage> storage,
                     std::shared_ptr<AlertManager> alert_manager,
                     std::string host,
                     int port,
                     std::string auth_user,
                     std::string auth_password,
                     std::shared_ptr<PiiMasker> pii_masker,
                     EnterpriseLicense license,
                     std::shared_ptr<EnterpriseUpdater> updater,
                     std::shared_ptr<AuditLedger> audit_ledger)
    : storage_(std::move(storage)),
      alert_manager_(std::move(alert_manager)),
      host_(std::move(host)),
      port_(port),
      auth_user_(std::move(auth_user)),
      auth_password_(std::move(auth_password)),
      pii_masker_(std::move(pii_masker)),
      license_(std::move(license)),
      updater_(std::move(updater)),
      audit_ledger_(std::move(audit_ledger)),
      start_time_(std::chrono::steady_clock::now()) {
    setup_routes();
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
    svr_.Get("/ready", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ready\n", "text/plain");
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
                        if (pii_masker_) pii_masker_->mask_entry(entry);
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
                            if (pii_masker_) pii_masker_->mask_entry(entry);
                            alert_manager_->record_log(entry);
                            entries.push_back(std::move(entry));
                        }
                    }
                } else {
                    LogEntry entry = LogEntry::from_json(j);
                    if (pii_masker_) pii_masker_->mask_entry(entry);
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
                    if (pii_masker_) pii_masker_->mask_entry(entry);
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
        if (audit_ledger_) audit_ledger_->record_event(get_client_ip(req), "INGEST", "count=" + std::to_string(count));

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

                            if (pii_masker_) pii_masker_->mask_entry(entry);
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

    // Loki-compatible Read API for Grafana Loki Data Source
    svr_.Get("/loki/api/v1/labels", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_content(R"({"status":"success","data":["service","level"]})", "application/json");
    });

    svr_.Get(R"(/loki/api/v1/label/([^/]+)/values)", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        std::string label = req.matches[1];
        nlohmann::json vals = nlohmann::json::array();
        if (label == "service" || label == "app" || label == "job") {
            for (const auto& s : storage_->get_stats().known_services) {
                vals.push_back(s);
            }
        } else if (label == "level") {
            vals = {"INFO", "WARN", "ERROR", "DEBUG", "FATAL"};
        }
        res.set_content(nlohmann::json{{"status", "success"}, {"data", vals}}.dump(), "application/json");
    });

    auto handle_loki_query = [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");

        SearchParams params;
        params.limit = 500;
        if (req.has_param("limit")) {
            try {
                params.limit = std::clamp<size_t>(std::stoull(req.get_param_value("limit")), 1, 1000);
            } catch (...) {}
        }

        auto parse_loki_ts_ms = [](const std::string& val) -> int64_t {
            try {
                int64_t n = std::stoll(val);
                if (n > 1000000000000000LL) return n / 1000000LL; // nanoseconds -> ms
                if (n > 100000000000LL) return n;                  // milliseconds
                return n * 1000LL;                                 // seconds -> ms
            } catch (...) { return 0; }
        };
        if (req.has_param("start")) params.from_ts = parse_loki_ts_ms(req.get_param_value("start"));
        if (req.has_param("end")) params.to_ts = parse_loki_ts_ms(req.get_param_value("end"));

        if (req.has_param("query")) {
            std::string lql = req.get_param_value("query");
            auto extract_label = [&](const std::string& key) -> std::string {
                for (const std::string& pat : {key + "=\"", key + "=~\"", key + "='"}) {
                    auto p = lql.find(pat);
                    if (p != std::string::npos) {
                        size_t s = p + pat.size();
                        char q = pat.back();
                        size_t e = lql.find(q, s);
                        if (e != std::string::npos) return lql.substr(s, e - s);
                    }
                }
                return "";
            };
            std::string svc = extract_label("service");
            if (svc.empty()) svc = extract_label("app");
            if (!svc.empty()) params.service = svc;

            std::string lvl = extract_label("level");
            if (!lvl.empty()) params.level = LogEntry::normalize_level(lvl);

            auto pipe_pos = lql.find("|=");
            if (pipe_pos != std::string::npos) {
                auto q1 = lql.find('"', pipe_pos);
                auto q2 = (q1 != std::string::npos) ? lql.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos) {
                    params.query = lql.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        SearchResult sr = storage_->search(params);
        std::map<std::pair<std::string, std::string>, nlohmann::json> streams;
        for (const auto& log : sr.logs) {
            auto key = std::make_pair(log.service, log.level);
            if (streams.find(key) == streams.end()) {
                streams[key] = nlohmann::json::array();
            }
            std::string ns = std::to_string(log.timestamp * 1000000LL);
            std::string line = log.raw_json.empty() ? log.message : log.raw_json;
            streams[key].push_back({ns, line});
        }

        nlohmann::json result_arr = nlohmann::json::array();
        for (auto& [k, vals] : streams) {
            result_arr.push_back({
                {"stream", {{"service", k.first}, {"level", k.second}}},
                {"values", std::move(vals)}
            });
        }

        nlohmann::json resp = {
            {"status", "success"},
            {"data", {
                {"resultType", "streams"},
                {"result", result_arr}
            }}
        };
        res.status = 200;
        res.set_content(resp.dump(), "application/json");
    };
    svr_.Get("/loki/api/v1/query_range", handle_loki_query);
    svr_.Get("/loki/api/v1/query", handle_loki_query);

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

        if (audit_ledger_) {
            audit_ledger_->record_event(get_client_ip(req), "SEARCH", "q=" + params.query + " service=" + params.service + " matched=" + std::to_string(search_result.total_matched));
        }

        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });

    // Histogram Timeline API
    svr_.Get("/api/v1/histogram", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        SearchParams params;
        if (req.has_param("q")) params.query = req.get_param_value("q");
        if (req.has_param("service")) params.service = req.get_param_value("service");
        if (req.has_param("level")) params.level = LogEntry::normalize_level(req.get_param_value("level"));
        if (req.has_param("from")) {
            try { params.from_ts = std::stoll(req.get_param_value("from")); } catch (...) {}
        }
        if (req.has_param("to")) {
            try { params.to_ts = std::stoll(req.get_param_value("to")); } catch (...) {}
        }

        int64_t step_seconds = 0;
        if (req.has_param("step")) {
            try { step_seconds = std::stoll(req.get_param_value("step")); } catch (...) {}
        }

        HistogramResult hist = storage_->get_histogram(params, step_seconds);

        nlohmann::json buckets_json = nlohmann::json::array();
        for (const auto& b : hist.buckets) {
            buckets_json.push_back(b.to_json());
        }

        nlohmann::json response = {
            {"step_seconds", hist.step_seconds},
            {"duration_ms", hist.duration_ms},
            {"buckets", buckets_json}
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
            {"pii_masking", pii_masker_ ? pii_masker_->is_enabled() : false},
            {"noise_filter_enabled", storage_->get_noise_filter() ? storage_->get_noise_filter()->is_enabled() : false},
            {"finops_saved_events", storage_->get_noise_filter() ? storage_->get_noise_filter()->get_saved_events() : 0},
            {"enterprise_plan", license_.plan},
            {"license_active", license_.is_active()},
            {"recent_alerts", alerts_json}
        };

        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });

    // Enterprise: License Info
    svr_.Get("/api/v1/enterprise/license", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");
        nlohmann::json resp = {
            {"plan", license_.plan},
            {"customer", license_.customer.empty() ? "Community Edition" : license_.customer},
            {"is_active", license_.is_active()},
            {"expires_at_ms", license_.expires_ms},
            {"features", {
                {"finops_dedup", license_.has_feature("finops_dedup")},
                {"rca", license_.has_feature("rca")},
                {"waterfall", license_.has_feature("waterfall")},
                {"audit_ledger", license_.has_feature("audit_ledger")},
                {"updater", license_.has_feature("updater")}
            }}
        };
        res.status = 200;
        res.set_content(resp.dump(), "application/json");
    });

    // Enterprise: Check Updates
    svr_.Get("/api/v1/enterprise/updates", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");
        if (!updater_) {
            res.status = 200;
            res.set_content(R"({"update_available":false,"message":"Updater not configured"})", "application/json");
            return;
        }
        auto check = updater_->check_update(license_.raw_token);
        res.status = 200;
        res.set_content(check.to_json().dump(), "application/json");
    });

    // Enterprise: Apply Update
    svr_.Post("/api/v1/enterprise/updates/apply", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        if (!updater_) {
            res.status = 400;
            res.set_content(R"({"error":"Updater not configured"})", "application/json");
            return;
        }
        std::string err;
        std::string url;
        try {
            auto j = nlohmann::json::parse(req.body);
            if (j.contains("download_url") && j["download_url"].is_string()) {
                url = j["download_url"].get<std::string>();
            }
        } catch (...) {}

        if (url.empty()) {
            auto check = updater_->check_update(license_.raw_token);
            url = check.download_url;
        }

        bool ok = updater_->apply_update(url, err);
        nlohmann::json resp = {
            {"success", ok},
            {"error", err}
        };
        res.status = ok ? 200 : 500;
        res.set_content(resp.dump(), "application/json");
    });

    // Enterprise: Audit Log
    svr_.Get("/api/v1/enterprise/audit", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        if (!license_.has_feature("audit_ledger")) {
            nlohmann::json locked = {
                {"status", "LOCKED"},
                {"tier_required", "enterprise"},
                {"audit_events", nlohmann::json::array()},
                {"message", "Audit ledger requires an Enterprise license."}
            };
            res.status = 200;
            res.set_content(locked.dump(), "application/json");
            return;
        }

        nlohmann::json arr = nlohmann::json::array();
        if (audit_ledger_) {
            for (const auto& ev : audit_ledger_->get_recent_events(50)) {
                arr.push_back(ev.to_json());
            }
        }
        res.status = 200;
        res.set_content(nlohmann::json{{"audit_events", arr}}.dump(), "application/json");
    });

    // Enterprise: Root Cause Analysis (RCA) Incident API
    svr_.Get("/api/v1/enterprise/incident/rca", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        if (!license_.has_feature("rca")) {
            nlohmann::json locked = {
                {"status", "LOCKED"},
                {"has_incident", false},
                {"tier_required", "business"},
                {"message", "Root Cause Analysis (RCA) requires a Business or Enterprise license."}
            };
            res.status = 200;
            res.set_content(locked.dump(), "application/json");
            return;
        }

        IncidentAnalysis incident;
        if (storage_->get_rca_engine()) {
            incident = storage_->get_rca_engine()->get_current_incident();
        }

        res.status = 200;
        res.set_content(incident.to_json().dump(), "application/json");
    });

    // Distributed Trace Stitching & Waterfall API
    svr_.Get("/api/v1/trace", [this](const httplib::Request& req, httplib::Response& res) {
        if (!check_auth(req, res)) return;
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");

        if (!req.has_param("trace_id") || req.get_param_value("trace_id").empty()) {
            res.status = 400;
            res.set_content(R"({"error":"missing trace_id parameter"})", "application/json");
            return;
        }

        std::string trace_id = req.get_param_value("trace_id");

        if (!license_.has_feature("waterfall")) {
            nlohmann::json locked = {
                {"error", "feature_locked"},
                {"trace_id", trace_id},
                {"tier_required", "business"},
                {"spans_count", 0},
                {"spans", nlohmann::json::array()},
                {"message", "Trace waterfall requires a Business or Enterprise license. Filter by trace_id in Community."}
            };
            res.status = 200;
            res.set_content(locked.dump(), "application/json");
            return;
        }
        std::vector<LogEntry> logs = storage_->get_trace(trace_id);

        int64_t start_ms = logs.empty() ? 0 : logs.front().timestamp;
        int64_t end_ms = logs.empty() ? 0 : logs.back().timestamp;
        int64_t total_duration = std::max<int64_t>(1, end_ms - start_ms);

        nlohmann::json spans = nlohmann::json::array();
        for (size_t i = 0; i < logs.size(); ++i) {
            const auto& entry = logs[i];
            int64_t offset = entry.timestamp - start_ms;
            int64_t dur = 1;
            if (i + 1 < logs.size()) {
                dur = std::max<int64_t>(1, logs[i + 1].timestamp - entry.timestamp);
            }
            spans.push_back({
                {"service", entry.service},
                {"level", entry.level},
                {"message", entry.message},
                {"timestamp_ms", entry.timestamp},
                {"offset_ms", offset},
                {"duration_ms", dur},
                {"repeat_count", entry.repeat_count}
            });
        }

        nlohmann::json resp = {
            {"trace_id", trace_id},
            {"spans_count", logs.size()},
            {"start_time_ms", start_ms},
            {"end_time_ms", end_ms},
            {"total_duration_ms", total_duration},
            {"spans", spans}
        };

        if (audit_ledger_) {
            audit_ledger_->record_event(get_client_ip(req), "TRACE_VIEW", "trace_id=" + trace_id + " spans=" + std::to_string(logs.size()));
        }

        res.status = 200;
        res.set_content(resp.dump(), "application/json");
    });
}

void ApiServer::start() {
    std::cout << "[server] listening on " << host_ << ":" << port_ << std::endl;
    if (!auth_user_.empty()) {
        std::cout << "[server] basic auth: enabled (user: " << auth_user_ << ")" << std::endl;
    }
    if (pii_masker_ && pii_masker_->is_enabled()) {
        std::cout << "[server] pii & secret masking: enabled" << std::endl;
    }
    if (license_.valid) {
        std::cout << "[server] enterprise plan: " << license_.plan << " (customer: " << license_.customer << ")" << std::endl;
    }
    svr_.listen(host_, port_);
}

void ApiServer::stop() {
    svr_.stop();
}
