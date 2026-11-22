#include "enterprise/license_manager.hpp"
#include <fstream>
#include <sstream>

static std::string base64_decode_str(std::string_view in) {
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

EnterpriseLicense LicenseManager::parse_license_file(const std::string& path) {
    EnterpriseLicense lic;
    std::ifstream file(path);
    if (!file.is_open()) {
        lic.error = "License file not found: " + path;
        return lic;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_license_token(buffer.str());
}

EnterpriseLicense LicenseManager::parse_license_token(const std::string& raw) {
    EnterpriseLicense lic;
    lic.raw_token = raw;

    std::string trimmed = raw;
    while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n' || trimmed.back() == ' ')) {
        trimmed.pop_back();
    }

    std::string json_str = trimmed;
    if (!trimmed.empty() && trimmed.front() != '{') {
        json_str = base64_decode_str(trimmed);
    }

    try {
        nlohmann::json j = nlohmann::json::parse(json_str);
        if (j.contains("customer") && j["customer"].is_string()) {
            lic.customer = j["customer"].get<std::string>();
        }
        if (j.contains("plan") && j["plan"].is_string()) {
            lic.plan = j["plan"].get<std::string>();
        }
        if (j.contains("expires_at_ms") && j["expires_at_ms"].is_number_integer()) {
            lic.expires_ms = j["expires_at_ms"].get<int64_t>();
        }

        if (j.contains("signature") && j["signature"].is_string() && !j["signature"].get<std::string>().empty()) {
            lic.valid = true;
        } else {
            lic.error = "License signature is missing";
        }
    } catch (const std::exception& e) {
        lic.error = std::string("Malformed license format: ") + e.what();
    }

    return lic;
}
