#pragma once

#include "log_entry.hpp"
#include "chunk_meta.hpp"
#include "search/query_parser.hpp"
#include "storage/s3_client.hpp"
#include "enterprise/noise_filter.hpp"
#include "enterprise/rca_engine.hpp"
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>

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

struct HistogramBucket {
    int64_t timestamp = 0;
    size_t total = 0;
    size_t info = 0;
    size_t warn = 0;
    size_t error = 0;

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"timestamp", timestamp},
            {"total", total},
            {"info", info},
            {"warn", warn},
            {"error", error}
        };
    }
};

struct HistogramResult {
    int64_t step_seconds = 60;
    std::vector<HistogramBucket> buckets;
    double duration_ms = 0.0;
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
                        std::chrono::milliseconds flush_interval = std::chrono::milliseconds(2000),
                        int retention_days = 0,
                        std::shared_ptr<S3Client> s3_client = nullptr,
                        std::shared_ptr<NoiseFilter> noise_filter = nullptr,
                        std::shared_ptr<RcaEngine> rca_engine = nullptr);
    ~LogStorage();

    void append(const LogEntry& entry);
    void append_batch(const std::vector<LogEntry>& entries);
    SearchResult search(const SearchParams& params);
    HistogramResult get_histogram(const SearchParams& params, int64_t step_seconds = 0);
    std::vector<LogEntry> get_trace(const std::string& trace_id);
    std::shared_ptr<NoiseFilter> get_noise_filter() const { return noise_filter_; }
    std::shared_ptr<RcaEngine> get_rca_engine() const { return rca_engine_; }
    StorageStats get_stats();
    void flush();
    void enforce_retention();
    void stop();

private:
    void background_flusher();
    void flush_batch_to_disk(const std::vector<LogEntry>& batch, const std::string& wal_flushing_path = "");
    void load_existing_chunks();
    void recover_wal_file(const std::string& path);

    static std::string get_date_string(int64_t ts_ms);
    static bool matches_filter(const LogEntry& log, const SearchParams& params, const LogSqlParser* parser = nullptr);
    static std::vector<LogEntry> search_chunk_file(const ChunkMeta& meta, const SearchParams& params, const LogSqlParser* parser = nullptr);

    std::string data_dir_;
    std::string wal_path_;
    std::string wal_flushing_path_;
    size_t max_batch_size_;
    std::chrono::milliseconds flush_interval_;
    int retention_days_;
    std::shared_ptr<S3Client> s3_client_;

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
    std::chrono::steady_clock::time_point last_retention_check_;
    std::shared_ptr<NoiseFilter> noise_filter_;
    std::shared_ptr<RcaEngine> rca_engine_;
};
