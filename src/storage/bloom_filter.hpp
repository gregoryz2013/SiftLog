#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

class BloomFilter {
public:
    static constexpr size_t NUM_WORDS = 128; // 128 * 64 = 8192 bits (1 KB)
    static constexpr size_t NUM_BITS = NUM_WORDS * 64;

    void clear() noexcept {
        bits_.fill(0);
    }

    void add_token(std::string_view token) noexcept {
        if (token.size() < 2) return;
        uint64_t h1 = fnv1a_lower(token);
        uint64_t h2 = splitmix64(h1 ^ 0x9e3779b97f4a7c15ULL);
        for (uint64_t i = 0; i < 3; ++i) {
            size_t bit = static_cast<size_t>((h1 + i * h2) % NUM_BITS);
            bits_[bit >> 6] |= (1ULL << (bit & 63));
        }
    }

    void add_text(std::string_view text) noexcept {
        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            while (i < n && !is_token_char(text[i])) ++i;
            size_t start = i;
            while (i < n && is_token_char(text[i])) ++i;
            if (i > start) {
                std::string_view tok = text.substr(start, i - start);
                add_token(tok);
                // Also index sub-tokens split on '-' or '_' so both "trc-8812" and "8812" match
                size_t sub_start = 0;
                for (size_t k = 0; k <= tok.size(); ++k) {
                    if (k == tok.size() || tok[k] == '-' || tok[k] == '_') {
                        if (k > sub_start && (k - sub_start) < tok.size()) {
                            add_token(tok.substr(sub_start, k - sub_start));
                        }
                        sub_start = k + 1;
                    }
                }
            }
        }
    }

    bool might_contain(std::string_view token) const noexcept {
        if (token.size() < 2) return true;
        uint64_t h1 = fnv1a_lower(token);
        uint64_t h2 = splitmix64(h1 ^ 0x9e3779b97f4a7c15ULL);
        for (uint64_t i = 0; i < 3; ++i) {
            size_t bit = static_cast<size_t>((h1 + i * h2) % NUM_BITS);
            if ((bits_[bit >> 6] & (1ULL << (bit & 63))) == 0) {
                return false;
            }
        }
        return true;
    }

    bool might_contain_text(std::string_view text) const noexcept {
        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            while (i < n && !is_token_char(text[i])) ++i;
            size_t start = i;
            while (i < n && is_token_char(text[i])) ++i;
            if (i > start) {
                if (!might_contain(text.substr(start, i - start))) {
                    return false;
                }
            }
        }
        return true;
    }

    std::string to_hex() const {
        static constexpr char hex_chars[] = "0123456789abcdef";
        std::string out;
        out.resize(NUM_WORDS * 16);
        for (size_t w = 0; w < NUM_WORDS; ++w) {
            uint64_t val = bits_[w];
            for (int shift = 60; shift >= 0; shift -= 4) {
                out[w * 16 + static_cast<size_t>((60 - shift) / 4)] = hex_chars[(val >> shift) & 0xF];
            }
        }
        return out;
    }

    bool from_hex(std::string_view hex) noexcept {
        if (hex.size() != NUM_WORDS * 16) return false;
        for (size_t w = 0; w < NUM_WORDS; ++w) {
            uint64_t val = 0;
            for (size_t j = 0; j < 16; ++j) {
                char c = hex[w * 16 + j];
                uint64_t nibble = 0;
                if (c >= '0' && c <= '9') nibble = static_cast<uint64_t>(c - '0');
                else if (c >= 'a' && c <= 'f') nibble = static_cast<uint64_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nibble = static_cast<uint64_t>(c - 'A' + 10);
                else return false;
                val = (val << 4) | nibble;
            }
            bits_[w] = val;
        }
        return true;
    }

private:
    static bool is_token_char(char c) noexcept {
        unsigned char uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || c == '_' || c == '-';
    }

    static uint64_t fnv1a_lower(std::string_view s) noexcept {
        uint64_t h = 14695981039346656037ULL;
        for (char c : s) {
            unsigned char lc = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)));
            h ^= lc;
            h *= 1099511628211ULL;
        }
        return h;
    }

    static uint64_t splitmix64(uint64_t x) noexcept {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    std::array<uint64_t, NUM_WORDS> bits_{};
};
