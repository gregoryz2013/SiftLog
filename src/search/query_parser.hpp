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

struct PipeSpec {
    enum class Kind { NONE, STATS_COUNT, TOP };
    Kind kind = Kind::NONE;
    std::string group_by = "service";
    size_t top_n = 10;
};

class LogSqlParser {
public:
    explicit LogSqlParser(std::string_view query_str);

    // Evaluates whether a log entry matches the parsed query filter expression
    bool matches(const LogEntry& entry) const;

    // Extracts positive search terms that must appear in any matching chunk (for Bloom pre-filtering)
    std::vector<std::string> required_terms() const;

    bool has_pipe() const noexcept { return pipe_.kind != PipeSpec::Kind::NONE; }
    const PipeSpec& pipe() const noexcept { return pipe_; }
    bool is_empty() const noexcept { return tokens_.empty() && pipe_.kind == PipeSpec::Kind::NONE; }

private:
    void tokenize(std::string_view query_str);
    void parse_pipe(std::string_view pipe_str);
    static bool wildcard_match(std::string_view text, std::string_view pat) noexcept;
    static std::string to_lower(std::string_view str);

    std::vector<QueryToken> tokens_;
    PipeSpec pipe_;
};
