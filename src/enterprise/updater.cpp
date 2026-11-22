#include "enterprise/updater.hpp"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>

static std::wstring to_wide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring w(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()), w.data(), size);
    return w;
}

static std::string http_get_sync(const std::string& host, int port, const std::string& path, bool https) {
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"SiftLogEnterpriseUpdater/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    std::wstring wHost = to_wide(host);
    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    std::wstring wPath = to_wide(path);
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (hRequest) {
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD dwSize = 0;
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;
                std::vector<char> buf(dwSize);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                    response.append(buf.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }
        WinHttpCloseHandle(hRequest);
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}
#else
static std::string http_get_sync(const std::string& host, int port, const std::string& path, bool https) {
    std::string proto = https ? "https" : "http";
    std::string url = proto + "://" + host + ":" + std::to_string(port) + path;
    std::string cmd = "curl -s -m 10 \"" + url + "\" 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    std::string result;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    return result;
}
#endif

EnterpriseUpdater::EnterpriseUpdater(std::string current_version, std::string update_server_url)
    : current_version_(std::move(current_version)),
      update_server_url_(std::move(update_server_url)) {
    if (update_server_url_.empty()) {
        update_server_url_ = "updates.siftlog.ru";
    }
}

UpdateCheckResult EnterpriseUpdater::check_update(const std::string& license_token) {
    UpdateCheckResult res;
    res.current_version = current_version_;

    std::string path = "/api/v1/version?current=" + current_version_;
    if (!license_token.empty()) {
        path += "&license=" + license_token;
    }

    std::string raw = http_get_sync(update_server_url_, 443, path, true);
    if (raw.empty()) {
        res.error = "Could not reach update server at " + update_server_url_;
        return res;
    }

    try {
        nlohmann::json j = nlohmann::json::parse(raw);
        if (j.contains("latest_version") && j["latest_version"].is_string()) {
            res.latest_version = j["latest_version"].get<std::string>();
            res.update_available = (res.latest_version != current_version_);
        }
        if (j.contains("download_url") && j["download_url"].is_string()) {
            res.download_url = j["download_url"].get<std::string>();
        }
        if (j.contains("release_notes") && j["release_notes"].is_string()) {
            res.release_notes = j["release_notes"].get<std::string>();
        }
    } catch (const std::exception& e) {
        res.error = std::string("Failed to parse update server response: ") + e.what();
    }

    return res;
}

bool EnterpriseUpdater::apply_update(const std::string& download_url, std::string& error_out) {
    if (download_url.empty()) {
        error_out = "No download URL provided";
        return false;
    }

    // On Linux/POSIX: Download via curl and atomically replace running executable
    // On Windows: Download to siftlog.new.exe and stage for restart
#if defined(_WIN32)
    std::string cmd = "powershell -Command \"Invoke-WebRequest -Uri '" + download_url + "' -OutFile 'siftlog-update.exe'\"";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        error_out = "Failed to download update binary";
        return false;
    }
    // Update staged
    return true;
#else
    std::string cmd = "curl -fsSL \"" + download_url + "\" -o siftlog.new && chmod +x siftlog.new && mv siftlog.new siftlog";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        error_out = "Failed to download and stage binary update";
        return false;
    }
    return true;
#endif
}
