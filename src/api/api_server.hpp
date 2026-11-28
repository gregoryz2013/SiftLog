#pragma once

#include "../storage/log_storage.hpp"
#include "../alerting/alert_manager.hpp"
#include "../security/pii_masker.hpp"
#include "../enterprise/license_manager.hpp"
#include "../enterprise/updater.hpp"
#include "../enterprise/audit_ledger.hpp"
#include <httplib.h>
#include <memory>
#include <chrono>

class ApiServer {
public:
    ApiServer(std::shared_ptr<LogStorage> storage,
              std::shared_ptr<AlertManager> alert_manager,
              std::string host = "0.0.0.0",
              int port = 8080,
              std::string auth_user = "",
              std::string auth_password = "",
              std::shared_ptr<PiiMasker> pii_masker = nullptr,
              EnterpriseLicense license = {},
              std::shared_ptr<EnterpriseUpdater> updater = nullptr,
              std::shared_ptr<AuditLedger> audit_ledger = nullptr,
              std::vector<std::string> peers = {},
              std::vector<std::string> replicate_to = {});

    void start();
    void stop();

private:
    void setup_routes();
    static std::string get_client_ip(const httplib::Request& req);
    bool check_auth(const httplib::Request& req, httplib::Response& res);
    void replicate_entries_async(const std::vector<LogEntry>& entries, const httplib::Request& req);
    nlohmann::json query_peer_json(const std::string& peer_url, const std::string& path_and_query, const httplib::Request& orig_req);

    std::shared_ptr<LogStorage> storage_;
    std::shared_ptr<AlertManager> alert_manager_;
    std::string host_;
    int port_;
    std::string auth_user_;
    std::string auth_password_;
    std::shared_ptr<PiiMasker> pii_masker_;
    EnterpriseLicense license_;
    std::shared_ptr<EnterpriseUpdater> updater_;
    std::shared_ptr<AuditLedger> audit_ledger_;
    std::vector<std::string> peers_;
    std::vector<std::string> replicate_to_;
    httplib::Server svr_;
    std::chrono::steady_clock::time_point start_time_;
};
