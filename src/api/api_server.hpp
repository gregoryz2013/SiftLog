#pragma once

#include "../storage/log_storage.hpp"
#include "../alerting/alert_manager.hpp"
#include <httplib.h>
#include <memory>
#include <chrono>

class ApiServer {
public:
    ApiServer(std::shared_ptr<LogStorage> storage,
              std::shared_ptr<AlertManager> alert_manager,
              std::string host = "0.0.0.0",
              int port = 8080);

    void start();
    void stop();

private:
    void setup_routes();
    static std::string get_client_ip(const httplib::Request& req);

    std::shared_ptr<LogStorage> storage_;
    std::shared_ptr<AlertManager> alert_manager_;
    std::string host_;
    int port_;
    httplib::Server svr_;
    std::chrono::steady_clock::time_point start_time_;
};
