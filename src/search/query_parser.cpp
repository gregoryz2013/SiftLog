#include "search/query_parser.hpp"
#include <algorithm>
#include <cctype>

std::string LogSqlParser::to_lower(std::string_view str) {
    std::string out;
    out.reserve(str.size());
    for (char c : str) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

LogSqlParser::LogSqlParser(std::string_view query_str) {
    tokenize(query_str);
}

void LogSqlParser::tokenize(std::string_view query_str) {
    tokens_.clear();
    size_t i = 0;
    const size_t n = query_str.size();

    while (i < n) {
        // Skip whitespace
        while (i < n && std::isspace(static_cast<unsigned char>(query_str[i]))) {
            ++i;
        }
        if (i >= n) break;

        // Check for NOT / !
        if (query_str[i] == '!') {
            tokens_.push_back({TokenType::NOT, "!", ""});
            ++i;
            continue;
        }

        // Quoted string
        if (query_str[i] == '"' || query_str[i] == '\'') {
            char quote = query_str[i++];
            size_t start = i;
            while (i < n && query_str[i] != quote) {
                ++i;
            }
            std::string text(query_str.substr(start, i - start));
            if (i < n) ++i; // skip closing quote
            tokens_.push_back({TokenType::STRING, to_lower(text), ""});
            continue;
        }

        // Read word
        size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(query_str[i])) && query_str[i] != '"' && query_str[i] != '\'') {
            ++i;
        }
        std::string_view word = query_str.substr(start, i - start);

        // Check keywords
        if (word == "AND" || word == "and" || word == "&&") {
            tokens_.push_back({TokenType::AND, "AND", ""});
        } else if (word == "OR" || word == "or" || word == "||") {
            tokens_.push_back({TokenType::OR, "OR", ""});
        } else if (word == "NOT" || word == "not") {
            tokens_.push_back({TokenType::NOT, "NOT", ""});
        } else {
            // Check field filter (e.g. service:auth or level:ERROR)
            auto colon = word.find(':');
            if (colon != std::string_view::npos && colon > 0 && colon + 1 < word.size()) {
                std::string field = to_lower(word.substr(0, colon));
                std::string val = to_lower(word.substr(colon + 1));
                tokens_.push_back({TokenType::FIELD_FILTER, val, field});
            } else {
                tokens_.push_back({TokenType::STRING, to_lower(word), ""});
            }
        }
    }
}

bool LogSqlParser::matches(const LogEntry& entry) const {
    if (tokens_.empty()) return true;

    std::string lower_msg = to_lower(entry.message);
    std::string lower_raw = to_lower(entry.raw_json);
    std::string lower_svc = to_lower(entry.service);
    std::string lower_lvl = to_lower(entry.level);

    auto eval_token = [&](const QueryToken& t) -> bool {
        if (t.type == TokenType::FIELD_FILTER) {
            if (t.field == "service" || t.field == "app") {
                return lower_svc == t.value || lower_svc.find(t.value) != std::string::npos;
            }
            if (t.field == "level" || t.field == "lvl") {
                return lower_lvl == t.value;
            }
            // Generic field search in raw json or message
            std::string needle = "\"" + t.field + "\":\"" + t.value + "\"";
            std::string needle_num = "\"" + t.field + "\":" + t.value;
            return lower_raw.find(needle) != std::string::npos ||
                   lower_raw.find(needle_num) != std::string::npos ||
                   lower_msg.find(t.value) != std::string::npos;
        }
        if (t.type == TokenType::STRING) {
            return lower_msg.find(t.value) != std::string::npos || lower_raw.find(t.value) != std::string::npos;
        }
        return true;
    };

    // Evaluator with support for AND, OR, NOT
    bool overall_result = true;
    bool current_or_branch = false;
    bool has_or = false;
    bool negate_next = false;

    for (size_t i = 0; i < tokens_.size(); ++i) {
        const auto& t = tokens_[i];

        if (t.type == TokenType::NOT) {
            negate_next = true;
            continue;
        }
        if (t.type == TokenType::AND) {
            continue; // AND is default conjunction
        }
        if (t.type == TokenType::OR) {
            has_or = true;
            continue;
        }

        bool match = eval_token(t);
        if (negate_next) {
            match = !match;
            negate_next = false;
        }

        if (has_or) {
            current_or_branch = current_or_branch || match;
            has_or = false;
        } else {
            if (i > 0 && (tokens_[i - 1].type == TokenType::STRING || tokens_[i - 1].type == TokenType::FIELD_FILTER)) {
                // Default implicit AND
                overall_result = overall_result && match;
            } else {
                current_or_branch = match;
            }
        }
    }

    return overall_result && current_or_branch;
}

std::vector<std::string> LogSqlParser::required_terms() const {
    std::vector<std::string> terms;
    // If query uses OR, we cannot safely require any single term across the whole chunk
    for (const auto& t : tokens_) {
        if (t.type == TokenType::OR) {
            return {};
        }
    }

    bool negate_next = false;
    for (const auto& t : tokens_) {
        if (t.type == TokenType::NOT) {
            negate_next = true;
            continue;
        }
        if (t.type == TokenType::AND) {
            continue;
        }
        if (negate_next) {
            negate_next = false;
            continue;
        }
        if (t.type == TokenType::STRING && t.value.size() >= 2) {
            terms.push_back(t.value);
        } else if (t.type == TokenType::FIELD_FILTER && t.field != "level" && t.field != "lvl" && t.value.size() >= 2) {
            terms.push_back(t.value);
        }
    }
    return terms;
}
