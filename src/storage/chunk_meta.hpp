#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <nlohmann/json.hpp>

struct ChunkMeta {
    std::string id;
    std::string chunk_file_path;
    std::string meta_file_path;
    int64_t min_ts = 0;
    int64_t max_ts = 0;
    size_t count = 0;
    std::unordered_set<std::string> services;
    std::unordered_set<std::string> levels;
    size_t uncompressed_bytes = 0;
    size_t compressed_bytes = 0;

    bool can_skip(int64_t from_ts, int64_t to_ts, const std::string& service_filter, const std::string& level_filter) const noexcept {
        if (from_ts > 0 && max_ts < from_ts) return true;
        if (to_ts > 0 && min_ts > to_ts) return true;
        if (!service_filter.empty() && services.find(service_filter) == services.end()) return true;
        if (!level_filter.empty() && levels.find(level_filter) == levels.end()) return true;
        return false;
    }

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"id", id},
            {"chunk_file", chunk_file_path},
            {"min_ts", min_ts},
            {"max_ts", max_ts},
            {"count", count},
            {"services", std::vector<std::string>(services.begin(), services.end())},
            {"levels", std::vector<std::string>(levels.begin(), levels.end())},
            {"uncompressed_bytes", uncompressed_bytes},
            {"compressed_bytes", compressed_bytes}
        };
    }

    static ChunkMeta from_json(const nlohmann::json& j, const std::string& meta_path) {
        ChunkMeta m;
        m.id = j.value("id", "");
        m.chunk_file_path = j.value("chunk_file", "");
        m.meta_file_path = meta_path;
        m.min_ts = j.value("min_ts", static_cast<int64_t>(0));
        m.max_ts = j.value("max_ts", static_cast<int64_t>(0));
        m.count = j.value("count", static_cast<size_t>(0));
        m.uncompressed_bytes = j.value("uncompressed_bytes", static_cast<size_t>(0));
        m.compressed_bytes = j.value("compressed_bytes", static_cast<size_t>(0));
        if (j.contains("services") && j["services"].is_array()) {
            for (const auto& s : j["services"]) {
                m.services.insert(s.get<std::string>());
            }
        }
        if (j.contains("levels") && j["levels"].is_array()) {
            for (const auto& l : j["levels"]) {
                m.levels.insert(l.get<std::string>());
            }
        }
        return m;
    }
};
