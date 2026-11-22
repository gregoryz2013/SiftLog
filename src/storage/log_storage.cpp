#include "log_storage.hpp"
#include <zstd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <future>
#include <algorithm>

namespace fs = std::filesystem;

LogStorage::LogStorage(std::string data_dir,
                       size_t max_batch_size,
                       std::chrono::milliseconds flush_interval,
                       int retention_days,
                       std::shared_ptr<S3Client> s3_client)
    : data_dir_(std::move(data_dir)),
      max_batch_size_(max_batch_size),
      flush_interval_(flush_interval),
      retention_days_(retention_days),
      s3_client_(std::move(s3_client)),
      last_retention_check_(std::chrono::steady_clock::now()) {
    fs::create_directories(data_dir_);
    load_existing_chunks();
    enforce_retention();
    flusher_thread_ = std::thread(&LogStorage::background_flusher, this);
}

LogStorage::~LogStorage() {
    stop();
}

void LogStorage::stop() {
    bool expected = true;
    if (running_.compare_exchange_strong(expected, false)) {
        cv_.notify_all();
        if (flusher_thread_.joinable()) {
            flusher_thread_.join();
        }
        flush();
    }
}

void LogStorage::append(const LogEntry& entry) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_batch_.push_back(entry);
        total_logs_ingested_.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> meta_lock(metadata_mutex_);
        known_services_.insert(entry.service);
    }
    if (current_batch_.size() >= max_batch_size_) {
        cv_.notify_one();
    }
}

void LogStorage::append_batch(const std::vector<LogEntry>& entries) {
    if (entries.empty()) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_batch_.insert(current_batch_.end(), entries.begin(), entries.end());
        total_logs_ingested_.fetch_add(entries.size(), std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> meta_lock(metadata_mutex_);
        for (const auto& e : entries) {
            known_services_.insert(e.service);
        }
    }
    if (current_batch_.size() >= max_batch_size_) {
        cv_.notify_one();
    }
}

void LogStorage::flush() {
    std::vector<LogEntry> batch_to_flush;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!current_batch_.empty()) {
            batch_to_flush.swap(current_batch_);
        }
    }
    if (!batch_to_flush.empty()) {
        flush_batch_to_disk(batch_to_flush);
    }
}

void LogStorage::enforce_retention() {
    if (retention_days_ <= 0) return;

    int64_t now_ms = LogEntry::now_ms();
    int64_t cutoff_ms = now_ms - (static_cast<int64_t>(retention_days_) * 24 * 3600 * 1000LL);

    std::lock_guard<std::mutex> lock(metadata_mutex_);
    size_t pruned_count = 0;
    std::unordered_set<std::string> checked_dirs;

    auto it = chunks_.begin();
    while (it != chunks_.end()) {
        if (it->max_ts < cutoff_ms) {
            // Delete files
            std::error_code ec;
            if (fs::exists(it->chunk_file_path, ec)) {
                fs::remove(it->chunk_file_path, ec);
            }
            if (fs::exists(it->meta_file_path, ec)) {
                checked_dirs.insert(fs::path(it->meta_file_path).parent_path().string());
                fs::remove(it->meta_file_path, ec);
            }

            total_compressed_bytes_ -= it->compressed_bytes;
            total_uncompressed_bytes_ -= it->uncompressed_bytes;
            it = chunks_.erase(it);
            ++pruned_count;
        } else {
            ++it;
        }
    }

    // Clean up empty parent date directories
    for (const auto& dir : checked_dirs) {
        std::error_code ec;
        if (fs::exists(dir, ec) && fs::is_empty(dir, ec)) {
            fs::remove(dir, ec);
        }
    }

    if (pruned_count > 0) {
        std::cout << "[retention] pruned " << pruned_count
                  << " chunks older than " << retention_days_ << " days" << std::endl;
    }
}

void LogStorage::background_flusher() {
    while (running_) {
        std::vector<LogEntry> batch_to_flush;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait_for(lock, flush_interval_, [&] {
                return !running_ || current_batch_.size() >= max_batch_size_;
            });

            if (!current_batch_.empty()) {
                batch_to_flush.swap(current_batch_);
            } else if (!running_) {
                break;
            }
        }

        if (!batch_to_flush.empty()) {
            flush_batch_to_disk(batch_to_flush);
        }

        // Check retention policy periodically (every 60s)
        auto now = std::chrono::steady_clock::now();
        if (retention_days_ > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - last_retention_check_).count() >= 60) {
            last_retention_check_ = now;
            enforce_retention();
        }
    }
}

std::string LogStorage::get_date_string(int64_t ts_ms) {
    std::time_t sec = static_cast<std::time_t>(ts_ms / 1000);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &sec);
#else
    gmtime_r(&sec, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d");
    return ss.str();
}

void LogStorage::flush_batch_to_disk(const std::vector<LogEntry>& batch) {
    if (batch.empty()) return;

    int64_t min_ts = batch.front().timestamp;
    int64_t max_ts = batch.front().timestamp;
    std::unordered_set<std::string> services;
    std::unordered_set<std::string> levels;

    std::string uncompressed_data;
    uncompressed_data.reserve(batch.size() * 128);

    for (const auto& log : batch) {
        min_ts = std::min(min_ts, log.timestamp);
        max_ts = std::max(max_ts, log.timestamp);
        services.insert(log.service);
        levels.insert(log.level);

        nlohmann::json item = {
            {"timestamp", log.timestamp},
            {"level", log.level},
            {"service", log.service},
            {"message", log.message},
            {"raw_json", log.raw_json}
        };
        uncompressed_data.append(item.dump());
        uncompressed_data.push_back('\n');
    }

    size_t uncompressed_size = uncompressed_data.size();
    size_t max_compressed_size = ZSTD_compressBound(uncompressed_size);
    std::vector<char> compressed_data(max_compressed_size);

    size_t compressed_size = ZSTD_compress(
        compressed_data.data(), max_compressed_size,
        uncompressed_data.data(), uncompressed_size,
        3
    );

    if (ZSTD_isError(compressed_size)) {
        std::cerr << "[storage] zstd compression error: "
                  << ZSTD_getErrorName(compressed_size) << std::endl;
        return;
    }

    std::string date_dir_name = get_date_string(max_ts);
    fs::path target_dir = fs::path(data_dir_) / date_dir_name;
    fs::create_directories(target_dir);

    int64_t now_ts = LogEntry::now_ms();
    std::string chunk_id = "chunk_" + std::to_string(now_ts) + "_" + std::to_string(min_ts);
    fs::path chunk_path = target_dir / (chunk_id + ".zst");
    fs::path meta_path = target_dir / (chunk_id + ".meta");

    {
        std::ofstream zst_file(chunk_path, std::ios::binary);
        if (!zst_file) {
            std::cerr << "[storage] failed to open chunk file for writing: " << chunk_path << std::endl;
            return;
        }
        zst_file.write(compressed_data.data(), static_cast<std::streamsize>(compressed_size));
    }

    ChunkMeta meta;
    meta.id = chunk_id;
    meta.chunk_file_path = chunk_path.string();
    meta.meta_file_path = meta_path.string();
    meta.min_ts = min_ts;
    meta.max_ts = max_ts;
    meta.count = batch.size();
    meta.services = std::move(services);
    meta.levels = std::move(levels);
    meta.uncompressed_bytes = uncompressed_size;
    meta.compressed_bytes = compressed_size;

    {
        std::ofstream meta_file(meta_path);
        if (meta_file) {
            meta_file << meta.to_json().dump(2);
        }
    }

    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        total_compressed_bytes_ += compressed_size;
        total_uncompressed_bytes_ += uncompressed_size;
        for (const auto& s : meta.services) {
            known_services_.insert(s);
        }
        chunks_.push_back(std::move(meta));
    }

    if (s3_client_ && s3_client_->is_enabled()) {
        std::string remote_zst = date_dir_name + "/" + chunk_id + ".zst";
        std::string remote_meta = date_dir_name + "/" + chunk_id + ".meta";
        s3_client_->upload_file_async(chunk_path.string(), remote_zst);
        s3_client_->upload_file_async(meta_path.string(), remote_meta);
    }
}

void LogStorage::load_existing_chunks() {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    chunks_.clear();
    known_services_.clear();
    total_compressed_bytes_ = 0;
    total_uncompressed_bytes_ = 0;

    if (!fs::exists(data_dir_)) return;

    for (const auto& entry : fs::recursive_directory_iterator(data_dir_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".meta") {
            try {
                std::ifstream f(entry.path());
                if (!f) continue;
                nlohmann::json j = nlohmann::json::parse(f);
                ChunkMeta meta = ChunkMeta::from_json(j, entry.path().string());

                if (!fs::exists(meta.chunk_file_path)) {
                    fs::path alt = entry.path().parent_path() / (meta.id + ".zst");
                    if (fs::exists(alt)) {
                        meta.chunk_file_path = alt.string();
                    }
                }

                total_compressed_bytes_ += meta.compressed_bytes;
                total_uncompressed_bytes_ += meta.uncompressed_bytes;
                for (const auto& s : meta.services) {
                    known_services_.insert(s);
                }
                chunks_.push_back(std::move(meta));
            } catch (const std::exception& e) {
                std::cerr << "[storage] failed to load meta " << entry.path() << ": " << e.what() << std::endl;
            }
        }
    }
}



bool LogStorage::matches_filter(const LogEntry& log, const SearchParams& params, const LogSqlParser* parser) {
    if (params.from_ts > 0 && log.timestamp < params.from_ts) return false;
    if (params.to_ts > 0 && log.timestamp > params.to_ts) return false;
    if (!params.service.empty() && log.service != params.service) return false;
    if (!params.level.empty() && log.level != params.level) return false;
    if (parser) {
        return parser->matches(log);
    }
    return true;
}

std::vector<LogEntry> LogStorage::search_chunk_file(const ChunkMeta& meta, const SearchParams& params, const LogSqlParser* parser) {
    std::vector<LogEntry> matches;
    std::ifstream file(meta.chunk_file_path, std::ios::binary);
    if (!file) return matches;

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (file_size == 0) return matches;

    std::vector<char> compressed(file_size);
    file.read(compressed.data(), static_cast<std::streamsize>(file_size));

    size_t uncompressed_capacity = meta.uncompressed_bytes > 0 ? meta.uncompressed_bytes : file_size * 10;
    std::vector<char> decompressed(uncompressed_capacity);
    size_t d_size = ZSTD_decompress(
        decompressed.data(), decompressed.size(),
        compressed.data(), compressed.size()
    );

    if (ZSTD_isError(d_size)) {
        std::cerr << "[storage] decompress error on " << meta.chunk_file_path
                  << ": " << ZSTD_getErrorName(d_size) << std::endl;
        return matches;
    }

    std::string_view sv(decompressed.data(), d_size);
    size_t start = 0;
    while (start < sv.size()) {
        size_t end = sv.find('\n', start);
        if (end == std::string_view::npos) end = sv.size();
        std::string_view line = sv.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;

        try {
            nlohmann::json j = nlohmann::json::parse(line);
            LogEntry entry;
            entry.timestamp = j.value("timestamp", static_cast<int64_t>(0));
            entry.level = j.value("level", "INFO");
            entry.service = j.value("service", "default");
            entry.message = j.value("message", "");
            entry.raw_json = j.value("raw_json", "");

            if (matches_filter(entry, params, parser)) {
                matches.push_back(std::move(entry));
            }
        } catch (...) {
        }
    }

    return matches;
}

SearchResult LogStorage::search(const SearchParams& params) {
    auto start_time = std::chrono::steady_clock::now();
    SearchResult result;

    std::unique_ptr<LogSqlParser> parser;
    if (!params.query.empty()) {
        parser = std::make_unique<LogSqlParser>(params.query);
    }

    std::vector<ChunkMeta> candidate_chunks;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        result.total_scanned_chunks = chunks_.size();
        for (const auto& ch : chunks_) {
            if (!ch.can_skip(params.from_ts, params.to_ts, params.service, params.level)) {
                candidate_chunks.push_back(ch);
            }
        }
    }
    result.matched_chunks = candidate_chunks.size();

    std::vector<LogEntry> all_matches;

    if (candidate_chunks.size() <= 2) {
        for (const auto& ch : candidate_chunks) {
            auto chunk_matches = search_chunk_file(ch, params, parser.get());
            all_matches.insert(all_matches.end(),
                               std::make_move_iterator(chunk_matches.begin()),
                               std::make_move_iterator(chunk_matches.end()));
        }
    } else {
        std::vector<std::future<std::vector<LogEntry>>> futures;
        futures.reserve(candidate_chunks.size());
        for (const auto& ch : candidate_chunks) {
            futures.push_back(std::async(std::launch::async, [ch, params, p = parser.get()]() {
                return search_chunk_file(ch, params, p);
            }));
        }
        for (auto& fut : futures) {
            auto chunk_matches = fut.get();
            all_matches.insert(all_matches.end(),
                               std::make_move_iterator(chunk_matches.begin()),
                               std::make_move_iterator(chunk_matches.end()));
        }
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& entry : current_batch_) {
            if (matches_filter(entry, params, parser.get())) {
                all_matches.push_back(entry);
            }
        }
    }

    result.total_matched = all_matches.size();

    std::sort(all_matches.begin(), all_matches.end(), [](const LogEntry& a, const LogEntry& b) {
        return a.timestamp > b.timestamp;
    });

    if (all_matches.size() > params.limit) {
        all_matches.resize(params.limit);
    }
    result.logs = std::move(all_matches);

    auto end_time = std::chrono::steady_clock::now();
    result.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    return result;
}

HistogramResult LogStorage::get_histogram(const SearchParams& params, int64_t step_seconds) {
    auto start_time = std::chrono::steady_clock::now();
    HistogramResult res;

    int64_t now_ms = LogEntry::now_ms();
    int64_t from_ts = params.from_ts > 0 ? params.from_ts : (now_ms - 3600 * 1000LL);
    int64_t to_ts = params.to_ts > 0 ? params.to_ts : now_ms;
    if (to_ts <= from_ts) {
        to_ts = from_ts + 3600 * 1000LL;
    }

    int64_t range_ms = to_ts - from_ts;
    int64_t step_ms = step_seconds > 0 ? (step_seconds * 1000LL) : std::max<int64_t>(1000LL, range_ms / 30LL);
    res.step_seconds = step_ms / 1000LL;

    size_t num_buckets = static_cast<size_t>(range_ms / step_ms) + 1;
    if (num_buckets > 500) num_buckets = 500;
    res.buckets.resize(num_buckets);
    for (size_t i = 0; i < num_buckets; ++i) {
        res.buckets[i].timestamp = from_ts + static_cast<int64_t>(i) * step_ms;
    }

    std::unique_ptr<LogSqlParser> parser;
    if (!params.query.empty()) {
        parser = std::make_unique<LogSqlParser>(params.query);
    }

    auto record_entry = [&](const LogEntry& entry) {
        if (!matches_filter(entry, params, parser.get())) return;
        if (entry.timestamp < from_ts || entry.timestamp > to_ts) return;
        size_t b_idx = static_cast<size_t>((entry.timestamp - from_ts) / step_ms);
        if (b_idx < res.buckets.size()) {
            res.buckets[b_idx].total++;
            if (entry.level == "ERROR" || entry.level == "FATAL") {
                res.buckets[b_idx].error++;
            } else if (entry.level == "WARN") {
                res.buckets[b_idx].warn++;
            } else {
                res.buckets[b_idx].info++;
            }
        }
    };

    std::vector<ChunkMeta> candidate_chunks;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        for (const auto& ch : chunks_) {
            if (!ch.can_skip(from_ts, to_ts, params.service, params.level)) {
                candidate_chunks.push_back(ch);
            }
        }
    }

    for (const auto& ch : candidate_chunks) {
        auto logs = search_chunk_file(ch, params, parser.get());
        for (const auto& log : logs) {
            record_entry(log);
        }
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& entry : current_batch_) {
            record_entry(entry);
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    res.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    return res;
}

StorageStats LogStorage::get_stats() {
    StorageStats stats;
    stats.total_logs_ingested = total_logs_ingested_.load(std::memory_order_relaxed);
    stats.disk_bytes_compressed = total_compressed_bytes_.load(std::memory_order_relaxed);
    stats.disk_bytes_uncompressed = total_uncompressed_bytes_.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(metadata_mutex_);
    stats.total_chunks_on_disk = chunks_.size();
    stats.known_services.assign(known_services_.begin(), known_services_.end());
    std::sort(stats.known_services.begin(), stats.known_services.end());
    return stats;
}
