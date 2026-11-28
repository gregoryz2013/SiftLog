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
    // Split on unquoted '|' for aggregation pipes (e.g. `level:ERROR | stats count() by service`)
    size_t pipe_pos = std::string_view::npos;
    char in_quote = 0;
    for (size_t i = 0; i < query_str.size(); ++i) {
        char c = query_str[i];
        if (in_quote) {
            if (c == in_quote) in_quote = 0;
        } else if (c == '"' || c == '\'') {
            in_quote = c;
        } else if (c == '|') {
            pipe_pos = i;
            break;
        }
    }

    if (pipe_pos != std::string_view::npos) {
        tokenize(query_str.substr(0, pipe_pos));
        parse_pipe(query_str.substr(pipe_pos + 1));
    } else {
        tokenize(query_str);
    }
}

void LogSqlParser::parse_pipe(std::string_view pipe_str) {
    std::string lower = to_lower(pipe_str);
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < lower.size()) {
        while (i < lower.size() && std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
        size_t s = i;
        while (i < lower.size() && !std::isspace(static_cast<unsigned char>(lower[i]))) ++i;
        if (i > s) parts.push_back(lower.substr(s, i - s));
    }
    if (parts.empty()) return;

    if (parts[0] == "stats") {
        pipe_.kind = PipeSpec::Kind::STATS_COUNT;
        pipe_.group_by = "service";
        for (size_t idx = 1; idx + 1 < parts.size(); ++idx) {
            if (parts[idx] == "by") {
                pipe_.group_by = parts[idx + 1];
                break;
            }
        }
    } else if (parts[0] == "top") {
        pipe_.kind = PipeSpec::Kind::TOP;
        pipe_.top_n = 10;
        pipe_.group_by = "message";
        if (parts.size() >= 2) {
            try {
                size_t n = std::stoull(parts[1]);
                pipe_.top_n = std::clamp<size_t>(n, 1, 100);
                if (parts.size() >= 3) pipe_.group_by = parts[2];
            } catch (...) {
                pipe_.group_by = parts[1];
            }
        }
    }
}

bool LogSqlParser::wildcard_match(std::string_view text, std::string_view pat) noexcept {
    size_t ti = 0, pi = 0, star = std::string_view::npos, match_ti = 0;
    while (ti < text.size()) {
        if (pi < pat.size() && (pat[pi] == text[ti] || pat[pi] == '?')) {
            ++ti;
            ++pi;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            match_ti = ti;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ti = ++match_ti;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
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
                if (t.value.find('*') != std::string::npos || t.value.find('?') != std::string::npos) {
                    return wildcard_match(lower_svc, t.value);
                }
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
            if (t.value.find('*') != std::string::npos || t.value.find('?') != std::string::npos) {
                return wildcard_match(lower_msg, t.value) || wildcard_match(lower_raw, t.value);
            }
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
        if (t.type == TokenType::STRING && t.value.size() >= 2 && t.value.find('*') == std::string::npos) {
            terms.push_back(t.value);
        } else if (t.type == TokenType::FIELD_FILTER && t.field != "level" && t.field != "lvl" &&
                   t.value.size() >= 2 && t.value.find('*') == std::string::npos) {
            terms.push_back(t.value);
        }
    }
    return terms;
}
