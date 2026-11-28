#include "security/pii_masker.hpp"
#include <algorithm>
#include <array>
#include <cctype>

namespace {

inline char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool is_digit_ascii(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline bool is_token_char(char c) noexcept {
    unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_' || c == '-' || c == '.' || c == '~' || c == '+' || c == '/' || c == '=';
}

inline bool is_word_boundary_left(std::string_view s, size_t pos) noexcept {
    if (pos == 0) return true;
    unsigned char c = static_cast<unsigned char>(s[pos - 1]);
    return !std::isalnum(c) && c != '_';
}

inline bool is_word_boundary_right(std::string_view s, size_t pos) noexcept {
    if (pos >= s.size()) return true;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    return !std::isalnum(c) && c != '_';
}

inline bool match_ci_at(std::string_view s, size_t pos, std::string_view pat) noexcept {
    if (pos + pat.size() > s.size()) return false;
    for (size_t i = 0; i < pat.size(); ++i) {
        if (to_lower_ascii(s[pos + i]) != pat[i]) return false;
    }
    return true;
}

std::string redact_bearer_and_jwt(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    const size_t n = input.size();

    while (i < n) {
        if (match_ci_at(input, i, "bearer") && is_word_boundary_left(input, i)) {
            size_t j = i + 6;
            size_t ws = j;
            while (j < n && (input[j] == ' ' || input[j] == '\t')) ++j;
            if (j > ws && j < n && is_token_char(input[j])) {
                while (j < n && is_token_char(input[j])) ++j;
                out.append("Bearer [MASKED_TOKEN]");
                i = j;
                continue;
            }
        }
        if (i + 20 < n && input[i] == 'e' && input[i + 1] == 'y' && input[i + 2] == 'J' && is_word_boundary_left(input, i)) {
            size_t p = i + 3;
            int dots = 0;
            size_t seg_len = 3;
            bool valid = true;
            while (p < n && is_token_char(input[p]) && input[p] != '~' && input[p] != '+' && input[p] != '/' && input[p] != '=') {
                if (input[p] == '.') {
                    if (seg_len < 4) { valid = false; break; }
                    ++dots;
                    seg_len = 0;
                } else {
                    ++seg_len;
                }
                ++p;
            }
            if (valid && dots == 2 && seg_len >= 4) {
                out.append("[MASKED_JWT]");
                i = p;
                continue;
            }
        }
        out.push_back(input[i++]);
    }
    return out;
}

std::string redact_secrets(std::string_view input) {
    static constexpr std::array<std::string_view, 9> keys = {
        "private_key", "private-key",
        "access_token", "access-token",
        "auth_token", "auth-token",
        "password", "passwd", "secret"
    };
    static constexpr std::array<std::string_view, 2> api_keys = {
        "api_key", "api-key"
    };

    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    const size_t n = input.size();

    while (i < n) {
        size_t matched_len = 0;
        if (is_word_boundary_left(input, i)) {
            for (auto k : keys) {
                if (match_ci_at(input, i, k) && is_word_boundary_right(input, i + k.size())) {
                    matched_len = k.size();
                    break;
                }
            }
            if (matched_len == 0) {
                for (auto k : api_keys) {
                    if (match_ci_at(input, i, k) && is_word_boundary_right(input, i + k.size())) {
                        matched_len = k.size();
                        break;
                    }
                }
            }
        }

        if (matched_len > 0) {
            size_t j = i + matched_len;
            // Optional closing quote on JSON key name before ':'
            size_t quote_key = j;
            if (quote_key < n && (input[quote_key] == '"' || input[quote_key] == '\'')) ++quote_key;
            while (quote_key < n && (input[quote_key] == ' ' || input[quote_key] == '\t')) ++quote_key;

            if (quote_key < n && (input[quote_key] == ':' || input[quote_key] == '=')) {
                size_t sep_end = quote_key + 1;
                while (sep_end < n && (input[sep_end] == ' ' || input[sep_end] == '\t')) ++sep_end;

                // Preserve optional backslash-escaped or plain quote prefix
                std::string quote_prefix;
                if (sep_end + 1 < n && input[sep_end] == '\\' && (input[sep_end + 1] == '"' || input[sep_end + 1] == '\'')) {
                    quote_prefix = std::string(input.substr(sep_end, 2));
                    sep_end += 2;
                } else if (sep_end < n && (input[sep_end] == '"' || input[sep_end] == '\'')) {
                    quote_prefix.push_back(input[sep_end]);
                    ++sep_end;
                }
                while (sep_end < n && (input[sep_end] == ' ' || input[sep_end] == '\t')) ++sep_end;

                size_t val_end = sep_end;
                while (val_end < n) {
                    char c = input[val_end];
                    if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' ||
                        c == ',' || c == '}' || c == ']' || c == '&' || c == '\\') {
                        break;
                    }
                    ++val_end;
                }

                if (val_end > sep_end) {
                    // Check if value wasn't already masked
                    std::string_view val_sv = input.substr(sep_end, val_end - sep_end);
                    if (val_sv != "[MASKED_SECRET]") {
                        out.append(input.substr(i, quote_key + 1 - i));
                        out.push_back(' ');
                        out.append(quote_prefix);
                        out.append("[MASKED_SECRET]");
                        i = val_end;
                        continue;
                    }
                }
            }
        }

        out.push_back(input[i++]);
    }
    return out;
}

std::string redact_numeric_pii(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    const size_t n = input.size();

    while (i < n) {
        if (is_digit_ascii(input[i]) && is_word_boundary_left(input, i)) {
            // 1. Credit Card: 4x4 with '-' or ' ' (19 chars) or 16 contiguous digits
            if (i + 19 <= n &&
                is_digit_ascii(input[i + 1]) && is_digit_ascii(input[i + 2]) && is_digit_ascii(input[i + 3]) &&
                (input[i + 4] == '-' || input[i + 4] == ' ') &&
                is_digit_ascii(input[i + 5]) && is_digit_ascii(input[i + 6]) && is_digit_ascii(input[i + 7]) && is_digit_ascii(input[i + 8]) &&
                input[i + 9] == input[i + 4] &&
                is_digit_ascii(input[i + 10]) && is_digit_ascii(input[i + 11]) && is_digit_ascii(input[i + 12]) && is_digit_ascii(input[i + 13]) &&
                input[i + 14] == input[i + 4] &&
                is_digit_ascii(input[i + 15]) && is_digit_ascii(input[i + 16]) && is_digit_ascii(input[i + 17]) && is_digit_ascii(input[i + 18]) &&
                is_word_boundary_right(input, i + 19)) {
                out.append("[MASKED_CARD]");
                i += 19;
                continue;
            }

            if (i + 16 <= n && is_word_boundary_right(input, i + 16)) {
                bool all16 = true;
                for (size_t k = 0; k < 16; ++k) {
                    if (!is_digit_ascii(input[i + k])) { all16 = false; break; }
                }
                if (all16) {
                    out.append("[MASKED_CARD]");
                    i += 16;
                    continue;
                }
            }

            // 2. SNILS: ddd-ddd-ddd dd (14 chars)
            if (i + 14 <= n &&
                is_digit_ascii(input[i + 1]) && is_digit_ascii(input[i + 2]) && input[i + 3] == '-' &&
                is_digit_ascii(input[i + 4]) && is_digit_ascii(input[i + 5]) && is_digit_ascii(input[i + 6]) && input[i + 7] == '-' &&
                is_digit_ascii(input[i + 8]) && is_digit_ascii(input[i + 9]) && is_digit_ascii(input[i + 10]) && input[i + 11] == ' ' &&
                is_digit_ascii(input[i + 12]) && is_digit_ascii(input[i + 13]) &&
                is_word_boundary_right(input, i + 14)) {
                out.append("[MASKED_SNILS]");
                i += 14;
                continue;
            }

            // 3. Passport: dddd dddddd (11 chars)
            if (i + 11 <= n &&
                is_digit_ascii(input[i + 1]) && is_digit_ascii(input[i + 2]) && is_digit_ascii(input[i + 3]) &&
                input[i + 4] == ' ' &&
                is_digit_ascii(input[i + 5]) && is_digit_ascii(input[i + 6]) && is_digit_ascii(input[i + 7]) &&
                is_digit_ascii(input[i + 8]) && is_digit_ascii(input[i + 9]) && is_digit_ascii(input[i + 10]) &&
                is_word_boundary_right(input, i + 11)) {
                out.append("[MASKED_PASSPORT]");
                i += 11;
                continue;
            }
        }

        out.push_back(input[i++]);
    }
    return out;
}

} // namespace

PiiMasker::PiiMasker(bool enabled) : enabled_(enabled) {}

std::string PiiMasker::mask_string(std::string_view input) const {
    if (!enabled_ || input.size() < 8) {
        return std::string(input);
    }

    std::string stage1 = redact_bearer_and_jwt(input);
    std::string stage2 = redact_secrets(stage1);

    bool has_digits = std::any_of(stage2.begin(), stage2.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
    if (has_digits) {
        return redact_numeric_pii(stage2);
    }
    return stage2;
}

void PiiMasker::mask_entry(LogEntry& entry) const {
    if (!enabled_) return;
    entry.message = mask_string(entry.message);
    if (!entry.raw_json.empty()) {
        entry.raw_json = mask_string(entry.raw_json);
    }
}
