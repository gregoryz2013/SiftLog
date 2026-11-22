#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include "../storage/log_entry.hpp"

// LogSQL token types
enum class TokenType {
    STRING,         // bare word or quoted phrase
    FIELD_FILTER,   // key:value (e.g. service:auth, level:ERROR)
    AND,            // AND
    OR,             // OR
    NOT             // NOT or !
};

struct QueryToken {
    TokenType type;
    std::string value;
    std::string field; // for FIELD_FILTER: field name (e.g. "service", "level")
};

class LogSqlParser {
public:
    explicit LogSqlParser(std::string_view query_str);

    // Evaluates whether a log entry matches the parsed LogSQL expression
    bool matches(const LogEntry& entry) const;

    bool is_empty() const noexcept { return tokens_.empty(); }

private:
    void tokenize(std::string_view query_str);
    static std::string to_lower(std::string_view str);

    std::vector<QueryToken> tokens_;
};
