#pragma once

#include "log_entry.hpp"
#include "chunk_meta.hpp"
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

struct SearchParams {
    std::string service;
    std::string level;
    std::string query;
    int64_t from_ts = 0;
    int64_t to_ts = 0;
    size_t limit = 100;
};

struct SearchResult {
    size_t total_scanned_chunks = 0;
    size_t matched_chunks = 0;
    size_t total_matched = 0;
    double duration_ms = 0.0;
    std::vector<LogEntry> logs;
};

struct StorageStats {
    size_t total_logs_ingested = 0;
    size_t total_chunks_on_disk = 0;
    size_t disk_bytes_compressed = 0;
    size_t disk_bytes_uncompressed = 0;
    std::vector<std::string> known_services;
};

class LogStorage {
public:
    explicit LogStorage(std::string data_dir,
                        size_t max_batch_size = 5000,
                        std::chrono::milliseconds flush_interval = std::chrono::milliseconds(2000));
    ~LogStorage();

    void append(const LogEntry& entry);
    void append_batch(const std::vector<LogEntry>& entries);
    SearchResult search(const SearchParams& params);
    StorageStats get_stats();
    void flush();
    void stop();

private:
    void background_flusher();
    void flush_batch_to_disk(const std::vector<LogEntry>& batch);
    void load_existing_chunks();

    static std::string get_date_string(int64_t ts_ms);
    static bool matches_filter(const LogEntry& log, const SearchParams& params);
    static std::vector<LogEntry> search_chunk_file(const ChunkMeta& meta, const SearchParams& params);

    std::string data_dir_;
    size_t max_batch_size_;
    std::chrono::milliseconds flush_interval_;

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::vector<LogEntry> current_batch_;
    std::atomic<bool> running_{true};
    std::thread flusher_thread_;

    std::mutex metadata_mutex_;
    std::vector<ChunkMeta> chunks_;
    std::unordered_set<std::string> known_services_;
    std::atomic<size_t> total_logs_ingested_{0};
    std::atomic<size_t> total_compressed_bytes_{0};
    std::atomic<size_t> total_uncompressed_bytes_{0};
};
