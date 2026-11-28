#include "storage/log_storage.hpp"
#include "storage/s3_client.hpp"
#include "alerting/alert_manager.hpp"
#include "api/api_server.hpp"
#include "security/pii_masker.hpp"
#include "enterprise/license_manager.hpp"
#include "enterprise/updater.hpp"
#include "enterprise/audit_ledger.hpp"
#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <memory>

static std::function<void()> g_shutdown_handler;

static void signal_handler(int sig) {
    std::cout << "\n[siftlog] received signal " << sig << ", shutting down..." << std::endl;
    if (g_shutdown_handler) {
        g_shutdown_handler();
    }
}

static std::string get_env(const char* name, const std::string& def) {
    const char* val = std::getenv(name);
    return (val && *val) ? std::string(val) : def;
}

int main(int argc, char* argv[]) {
    std::string host = get_env("SIFTLOG_HOST", "0.0.0.0");
    int port = 8080;
    std::string data_dir = get_env("SIFTLOG_DATA_DIR", "./data");
    std::string tg_token = get_env("SIFTLOG_TG_TOKEN", "");
    std::string tg_chat_id = get_env("SIFTLOG_TG_CHAT_ID", "");
    std::string alert_webhook = get_env("SIFTLOG_ALERT_WEBHOOK", "");
    size_t error_threshold = 10;
    int retention_days = 0;
    std::string auth_user = get_env("SIFTLOG_AUTH_USER", "");
    std::string auth_password = get_env("SIFTLOG_AUTH_PASSWORD", "");
    bool mask_pii = true;
    std::string mask_env = get_env("SIFTLOG_MASK_PII", "true");
    if (mask_env == "false" || mask_env == "0" || mask_env == "off") {
        mask_pii = false;
    }
    bool noise_dedup = true;
    std::string noise_env = get_env("SIFTLOG_NOISE_DEDUP", "true");
    if (noise_env == "false" || noise_env == "0" || noise_env == "off") {
        noise_dedup = false;
    }
    int noise_window = 5;
    std::string license_path = get_env("SIFTLOG_LICENSE", "");
    std::string update_url = get_env("SIFTLOG_UPDATE_URL", "");
    std::string audit_log_path = get_env("SIFTLOG_AUDIT_LOG", data_dir + "/audit.log");

    S3Config s3_cfg;
    s3_cfg.endpoint = get_env("SIFTLOG_S3_ENDPOINT", "");
    s3_cfg.bucket = get_env("SIFTLOG_S3_BUCKET", "");
    s3_cfg.region = get_env("SIFTLOG_S3_REGION", "us-east-1");
    s3_cfg.access_key = get_env("SIFTLOG_S3_ACCESS_KEY", "");
    s3_cfg.secret_key = get_env("SIFTLOG_S3_SECRET_KEY", "");
    s3_cfg.prefix = get_env("SIFTLOG_S3_PREFIX", "siftlog");
    s3_cfg.enabled = !s3_cfg.bucket.empty();

    try {
        std::string p_env = get_env("SIFTLOG_PORT", "");
        if (!p_env.empty()) port = std::stoi(p_env);
    } catch (...) {}

    try {
        std::string t_env = get_env("SIFTLOG_ERROR_THRESHOLD", "");
        if (!t_env.empty()) error_threshold = std::stoull(t_env);
    } catch (...) {}

    try {
        std::string r_env = get_env("SIFTLOG_RETENTION_DAYS", "");
        if (!r_env.empty()) retention_days = std::stoi(r_env);
    } catch (...) {}

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--tg-token" && i + 1 < argc) {
            tg_token = argv[++i];
        } else if (arg == "--tg-chat-id" && i + 1 < argc) {
            tg_chat_id = argv[++i];
        } else if (arg == "--alert-webhook" && i + 1 < argc) {
            alert_webhook = argv[++i];
        } else if (arg == "--error-threshold" && i + 1 < argc) {
            error_threshold = std::stoull(argv[++i]);
        } else if (arg == "--retention-days" && i + 1 < argc) {
            retention_days = std::stoi(argv[++i]);
        } else if (arg == "--auth-user" && i + 1 < argc) {
            auth_user = argv[++i];
        } else if (arg == "--auth-password" && i + 1 < argc) {
            auth_password = argv[++i];
        } else if (arg == "--license" && i + 1 < argc) {
            license_path = argv[++i];
        } else if (arg == "--update-url" && i + 1 < argc) {
            update_url = argv[++i];
        } else if (arg == "--audit-log" && i + 1 < argc) {
            audit_log_path = argv[++i];
        } else if (arg == "--s3-endpoint" && i + 1 < argc) {
            s3_cfg.endpoint = argv[++i];
        } else if (arg == "--s3-bucket" && i + 1 < argc) {
            s3_cfg.bucket = argv[++i];
            s3_cfg.enabled = true;
        } else if (arg == "--s3-region" && i + 1 < argc) {
            s3_cfg.region = argv[++i];
        } else if (arg == "--s3-access-key" && i + 1 < argc) {
            s3_cfg.access_key = argv[++i];
        } else if (arg == "--s3-secret-key" && i + 1 < argc) {
            s3_cfg.secret_key = argv[++i];
        } else if (arg == "--s3-prefix" && i + 1 < argc) {
            s3_cfg.prefix = argv[++i];
        } else if (arg == "--no-mask-pii") {
            mask_pii = false;
        } else if (arg == "--mask-pii") {
            mask_pii = true;
        } else if (arg == "--no-noise-dedup") {
            noise_dedup = false;
        } else if (arg == "--noise-window" && i + 1 < argc) {
            noise_window = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  --host <ip>                  Listen host (default: 0.0.0.0, use 127.0.0.1 behind reverse proxy)\n"
                      << "  --port <port>                Listen port (default: 8080)\n"
                      << "  --data-dir <path>            Storage directory (default: ./data)\n"
                      << "  --retention-days <days>      Auto-prune log chunks older than N days (default: 0 = disabled)\n"
                      << "  --auth-user <username>       HTTP Basic Auth username for UI & API (optional)\n"
                      << "  --auth-password <password>   HTTP Basic Auth password for UI & API (optional)\n"
                      << "  --s3-endpoint <url>          S3 endpoint (e.g. storage.yandexcloud.net, s3.amazonaws.com)\n"
                      << "  --s3-bucket <bucket>         S3 bucket name for log chunk storage\n"
                      << "  --s3-region <region>         S3 bucket region (default: us-east-1)\n"
                      << "  --s3-access-key <key>        S3 access key ID\n"
                      << "  --s3-secret-key <key>        S3 secret access key\n"
                      << "  --s3-prefix <prefix>         S3 object key prefix (default: siftlog)\n"
                      << "  --audit-log <path>           Audit trail file path (default: <data-dir>/audit.log)\n"
                      << "  --no-mask-pii                Disable automatic PII & secret masking (default: enabled)\n"
                      << "  --no-noise-dedup             Disable FinOps sliding-window noise deduplication (default: enabled)\n"
                      << "  --noise-window <sec>         FinOps noise deduplication sliding window in seconds (default: 5)\n"
                      << "  --tg-token <token>           Telegram Bot token for alerts\n"
                      << "  --tg-chat-id <id>            Telegram Chat ID for alerts\n"
                      << "  --alert-webhook <url>        Webhook URL for error spike alerts (Slack, Discord, Alertmanager)\n"
                      << "  --error-threshold <n>        Error spike threshold per 60s window (default: 10)\n"
                      << "  --help, -h                   Show this help message\n";
            return 0;
        }
    }

    std::cout << "[siftlog] version 0.5.0" << std::endl;
    std::cout << "[siftlog] host: " << host << ", port: " << port << std::endl;
    std::cout << "[siftlog] data_dir: " << data_dir << std::endl;
    if (retention_days > 0) {
        std::cout << "[siftlog] retention: " << retention_days << " days" << std::endl;
    } else {
        std::cout << "[siftlog] retention: disabled (infinite)" << std::endl;
    }
    std::cout << "[siftlog] auth: " << (auth_user.empty() ? "disabled (open)" : "basic auth enabled") << std::endl;
    std::cout << "[siftlog] pii & secret masking: " << (mask_pii ? "enabled" : "disabled") << std::endl;
    std::cout << "[siftlog] finops noise deduplication: " << (noise_dedup ? "enabled (window: " + std::to_string(noise_window) + "s)" : "disabled") << std::endl;
    if (s3_cfg.enabled) {
        std::cout << "[siftlog] s3 sync: enabled (bucket: " << s3_cfg.bucket << ", endpoint: " << s3_cfg.endpoint << ")" << std::endl;
    } else {
        std::cout << "[siftlog] s3 sync: disabled (local storage only)" << std::endl;
    }
    std::cout << "[siftlog] error_threshold: " << error_threshold << " errors/60s" << std::endl;
    std::cout << "[siftlog] telegram alerts: " << (tg_token.empty() ? "disabled" : "enabled") << std::endl;

    EnterpriseLicense license;
    if (!license_path.empty()) {
        license = LicenseManager::parse_license_file(license_path);
        if (!license.valid) {
            license = LicenseManager::parse_license_token(license_path);
        }
        std::cout << "[siftlog] enterprise license: " << (license.is_active() ? "ACTIVE (" + license.plan + ", customer: " + license.customer + ")" : "INACTIVE/EXPIRED") << std::endl;
    }

    auto s3_client = std::make_shared<S3Client>(s3_cfg);
    bool finops_enabled = noise_dedup && license.has_feature("finops_dedup");
    auto noise_filter = std::make_shared<NoiseFilter>(finops_enabled, noise_window);
    auto rca_engine = std::make_shared<RcaEngine>(60, 3);
    auto storage = std::make_shared<LogStorage>(data_dir, 20000, std::chrono::milliseconds(2000), retention_days, s3_client, noise_filter, rca_engine);
    auto alert_manager = std::make_shared<AlertManager>(tg_token, tg_chat_id, error_threshold, 60, 60, alert_webhook);
    auto pii_masker = std::make_shared<PiiMasker>(mask_pii);
    auto updater = std::make_shared<EnterpriseUpdater>("0.5.0", update_url);
    auto audit_ledger = std::make_shared<AuditLedger>(audit_log_path);
    auto server = std::make_shared<ApiServer>(storage, alert_manager, host, port, auth_user, auth_password, pii_masker, license, updater, audit_ledger);

    g_shutdown_handler = [&]() {
        server->stop();
        storage->stop();
    };

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server->start();
    storage->stop();
    return 0;
}
