#include "security/pii_masker.hpp"
#include <algorithm>

PiiMasker::PiiMasker(bool enabled)
    : enabled_(enabled),
      token_regex_(R"(Bearer\s+[A-Za-z0-9_\-\.~+/]+=*)", std::regex_constants::icase | std::regex_constants::optimize),
      jwt_regex_(R"(\beyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]+)", std::regex_constants::optimize),
      secret_regex_(R"((password|passwd|secret|api[_-]?key|access[_-]?token|auth[_-]?token|private[_-]?key)\s*([:=])\s*(\\?["']?\s*)([^\s"',\}\]&]+)(\\?["']?))", std::regex_constants::icase | std::regex_constants::optimize),
      card_regex_(R"(\b\d{4}[ -]\d{4}[ -]\d{4}[ -]\d{4}\b|\b\d{16}\b)", std::regex_constants::optimize),
      passport_regex_(R"(\b\d{4}\s\d{6}\b)", std::regex_constants::optimize),
      snils_regex_(R"(\b\d{3}-\d{3}-\d{3}\s\d{2}\b)", std::regex_constants::optimize) {}

std::string PiiMasker::mask_string(const std::string& input) const {
    if (!enabled_ || input.size() < 8) {
        return input;
    }

    std::string result = input;

    // 1. Mask Bearer and JWT tokens
    if (result.find("Bearer") != std::string::npos || result.find("bearer") != std::string::npos) {
        result = std::regex_replace(result, token_regex_, "Bearer [MASKED_TOKEN]");
    }
    if (result.find("eyJ") != std::string::npos) {
        result = std::regex_replace(result, jwt_regex_, "[MASKED_JWT]");
    }

    // 2. Fast check for keywords before executing complex secret regex
    auto contains_ci = [](std::string_view str, std::string_view sub) {
        auto it = std::search(str.begin(), str.end(), sub.begin(), sub.end(),
            [](char ch1, char ch2) { return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2)); });
        return it != str.end();
    };

    if (contains_ci(result, "pass") || contains_ci(result, "secret") || contains_ci(result, "key") || contains_ci(result, "token")) {
        result = std::regex_replace(result, secret_regex_, "$1$2 $3[MASKED_SECRET]$5");
    }

    // 3. Fast check for digits before checking cards, passport, and snils
    bool has_digits = std::any_of(result.begin(), result.end(), [](unsigned char c) {
        return std::isdigit(c);
    });

    if (has_digits) {
        result = std::regex_replace(result, card_regex_, "[MASKED_CARD]");
        result = std::regex_replace(result, passport_regex_, "[MASKED_PASSPORT]");
        result = std::regex_replace(result, snils_regex_, "[MASKED_SNILS]");
    }

    return result;
}

void PiiMasker::mask_entry(LogEntry& entry) const {
    if (!enabled_) return;
    entry.message = mask_string(entry.message);
    if (!entry.raw_json.empty()) {
        entry.raw_json = mask_string(entry.raw_json);
    }
}
