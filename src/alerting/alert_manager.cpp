#include "alert_manager.hpp"
#include <iostream>
#include <future>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>

static std::wstring to_wide_str(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), wstr.data(), size_needed);
    return wstr;
}
#endif

AlertManager::AlertManager(std::string tg_token,
                           std::string tg_chat_id,
                           size_t error_threshold,
                           int window_seconds,
                           int cooldown_seconds,
                           std::string webhook_url)
    : tg_token_(std::move(tg_token)),
      tg_chat_id_(std::move(tg_chat_id)),
      error_threshold_(error_threshold),
      window_seconds_(window_seconds),
      cooldown_seconds_(cooldown_seconds),
      webhook_url_(std::move(webhook_url)) {}

void AlertManager::record_log(const LogEntry& log) {
    if (log.level != "ERROR" && log.level != "FATAL") {
        return;
    }

    int64_t now = LogEntry::now_ms();
    int64_t cutoff = now - (window_seconds_ * 1000LL);
    bool should_alert = false;
    size_t error_count = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& timestamps = service_error_timestamps_[log.service];

        while (!timestamps.empty() && timestamps.front() < cutoff) {
            timestamps.pop_front();
        }

        timestamps.push_back(now);
        error_count = timestamps.size();

        if (error_count >= error_threshold_) {
            int64_t last_time = last_alert_time_[log.service];
            int64_t cooldown_ms = cooldown_seconds_ * 1000LL;
            if (now - last_time >= cooldown_ms) {
                last_alert_time_[log.service] = now;
                should_alert = true;
            }
        }
    }

    if (should_alert) {
        trigger_alert(log.service, error_count, log.message);
    }
}

void AlertManager::trigger_alert(const std::string& service, size_t error_count, const std::string& sample_error) {
    int64_t now_ts = LogEntry::now_ms();
    std::time_t sec = static_cast<std::time_t>(now_ts / 1000);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S UTC");
    std::string time_str = ss.str();

    AlertEvent event{
        service,
        error_count,
        now_ts,
        time_str,
        sample_error
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        recent_alerts_.push_back(event);
        if (recent_alerts_.size() > 50) {
            recent_alerts_.erase(recent_alerts_.begin());
        }
    }

    std::cout << "[alert] spike detected: service=" << service
              << " errors=" << error_count
              << " window=" << window_seconds_ << "s"
              << " threshold=" << error_threshold_
              << " sample=" << sample_error.substr(0, 80) << std::endl;

    if (!tg_token_.empty() && !tg_chat_id_.empty()) {
        std::thread([this, service, error_count, sample_error]() {
            send_telegram_alert(service, error_count, sample_error);
        }).detach();
    }
    if (!webhook_url_.empty()) {
        std::thread([this, service, error_count, sample_error]() {
            send_webhook_alert(service, error_count, sample_error);
        }).detach();
    }
}

void AlertManager::send_telegram_alert(const std::string& service, size_t count, const std::string& sample_error) {
    std::string msg_text = "[SiftLog Alert]\n"
                           "Service: " + service + "\n"
                           "Errors: " + std::to_string(count) + " in " + std::to_string(window_seconds_) + "s\n"
                           "Threshold: " + std::to_string(error_threshold_) + "\n"
                           "Sample: " + sample_error.substr(0, 200);

    nlohmann::json payload = {
        {"chat_id", tg_chat_id_},
        {"text", msg_text}
    };
    std::string body = payload.dump();

#if defined(_WIN32)
    HINTERNET hSession = WinHttpOpen(L"SiftLog/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.telegram.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return;
    }

    std::wstring path = L"/bot" + to_wide_str(tg_token_) + L"/sendMessage";
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    LPCWSTR headers = L"Content-Type: application/json\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1L),
                                   const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()), 0);
    if (sent) {
        WinHttpReceiveResponse(hRequest, nullptr);
    } else {
        std::cerr << "[alert] telegram dispatch error: winhttp " << GetLastError() << std::endl;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
#else
    // POSIX fallback using curl CLI
    std::string escaped_body;
    for (char c : body) {
        if (c == '\'') {
            escaped_body += "'\\''";
        } else {
            escaped_body += c;
        }
    }
    std::string cmd = "curl -s -m 5 -X POST https://api.telegram.org/bot" + tg_token_ +
                      "/sendMessage -H 'Content-Type: application/json' -d '" + escaped_body + "' >/dev/null 2>&1";
    int ret = std::system(cmd.c_str());
    (void)ret;
#endif
}

std::vector<AlertEvent> AlertManager::get_recent_alerts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recent_alerts_;
}

void AlertManager::send_webhook_alert(const std::string& service, size_t count, const std::string& sample_error) {
    nlohmann::json payload = {
        {"source", "siftlog"},
        {"service", service},
        {"error_count", count},
        {"window_seconds", window_seconds_},
        {"threshold", error_threshold_},
        {"sample_error", sample_error.substr(0, 300)},
        {"text", "[SiftLog Alert] service=" + service + " errors=" + std::to_string(count) +
                 "/" + std::to_string(window_seconds_) + "s: " + sample_error.substr(0, 200)}
    };
    std::string body = payload.dump();

#if defined(_WIN32)
    std::string url = webhook_url_;
    bool https = (url.rfind("https://", 0) == 0);
    size_t host_start = https ? 8 : (url.rfind("http://", 0) == 0 ? 7 : 0);
    if (host_start == 0) return;

    size_t slash_pos = url.find('/', host_start);
    std::string host_port = (slash_pos == std::string::npos) ? url.substr(host_start) : url.substr(host_start, slash_pos - host_start);
    std::string path = (slash_pos == std::string::npos) ? "/" : url.substr(slash_pos);
    INTERNET_PORT port = https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    std::string host = host_port;
    size_t colon = host_port.find(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        try { port = static_cast<INTERNET_PORT>(std::stoi(host_port.substr(colon + 1))); } catch (...) {}
    }

    HINTERNET hSession = WinHttpOpen(L"SiftLog/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;
    HINTERNET hConnect = WinHttpConnect(hSession, to_wide_str(host).c_str(), port, 0);
    if (hConnect) {
        DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", to_wide_str(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (hRequest) {
            LPCWSTR headers = L"Content-Type: application/json\r\n";
            if (WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1L), const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)) {
                WinHttpReceiveResponse(hRequest, nullptr);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
#else
    std::string escaped_body;
    for (char c : body) {
        if (c == '\'') escaped_body += "'\\''";
        else escaped_body += c;
    }
    std::string cmd = "curl -s -m 5 -X POST '" + webhook_url_ + "' -H 'Content-Type: application/json' -d '" + escaped_body + "' >/dev/null 2>&1";
    int ret = std::system(cmd.c_str());
    (void)ret;
#endif
}
