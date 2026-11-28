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
                       std::shared_ptr<S3Client> s3_client,
                       std::shared_ptr<NoiseFilter> noise_filter,
                       std::shared_ptr<RcaEngine> rca_engine)
    : data_dir_(std::move(data_dir)),
      wal_path_((fs::path(data_dir_) / "wal.log").string()),
      wal_flushing_path_((fs::path(data_dir_) / "wal.flushing").string()),
      max_batch_size_(max_batch_size),
      flush_interval_(flush_interval),
      retention_days_(retention_days),
      s3_client_(std::move(s3_client)),
      noise_filter_(std::move(noise_filter)),
      rca_engine_(std::move(rca_engine)),
      last_retention_check_(std::chrono::steady_clock::now()) {
    if (!noise_filter_) {
        noise_filter_ = std::make_shared<NoiseFilter>(true, 5);
    }
    if (!rca_engine_) {
        rca_engine_ = std::make_shared<RcaEngine>(60, 3);
    }
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
    append_batch({entry});
}

void LogStorage::append_batch(const std::vector<LogEntry>& entries) {
    if (entries.empty()) return;

    std::vector<LogEntry> processed = entries;
    if (noise_filter_) {
        processed = noise_filter_->dedup_batch(std::move(processed));
    }
    if (rca_engine_) {
        rca_engine_->observe(processed);
    }

    if (processed.empty()) return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        std::ofstream wal(wal_path_, std::ios::app | std::ios::binary);
        if (wal.is_open()) {
            for (const auto& e : processed) {
                wal << e.to_json().dump() << '\n';
            }
            wal.flush();
        }
        current_batch_.insert(current_batch_.end(), processed.begin(), processed.end());
        total_logs_ingested_.fetch_add(processed.size(), std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> meta_lock(metadata_mutex_);
        for (const auto& e : processed) {
            known_services_.insert(e.service);
        }
    }
    if (current_batch_.size() >= max_batch_size_) {
        cv_.notify_one();
    }
}

void LogStorage::flush() {
    std::vector<LogEntry> batch_to_flush;
    std::string rotated_wal;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!current_batch_.empty()) {
            batch_to_flush.swap(current_batch_);
            std::error_code ec;
            if (fs::exists(wal_path_, ec)) {
                fs::rename(wal_path_, wal_flushing_path_, ec);
                if (!ec) rotated_wal = wal_flushing_path_;
            }
        }
    }
    if (!batch_to_flush.empty()) {
        flush_batch_to_disk(batch_to_flush, rotated_wal);
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
        std::string rotated_wal;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait_for(lock, flush_interval_, [&] {
                return !running_ || current_batch_.size() >= max_batch_size_;
            });

            if (!current_batch_.empty()) {
                batch_to_flush.swap(current_batch_);
                std::error_code ec;
                if (fs::exists(wal_path_, ec)) {
                    fs::rename(wal_path_, wal_flushing_path_, ec);
                    if (!ec) rotated_wal = wal_flushing_path_;
                }
            } else if (!running_) {
                break;
            }
        }

        if (!batch_to_flush.empty()) {
            flush_batch_to_disk(batch_to_flush, rotated_wal);
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

void LogStorage::flush_batch_to_disk(const std::vector<LogEntry>& batch, const std::string& wal_flushing_path) {
    if (batch.empty()) return;

    int64_t min_ts = batch.front().timestamp;
    int64_t max_ts = batch.front().timestamp;
    std::unordered_set<std::string> services;
    std::unordered_set<std::string> levels;
    std::unordered_map<std::string, size_t> service_counts;
    std::unordered_map<std::string, size_t> level_counts;
    BloomFilter bloom;

    std::string uncompressed_data;
    uncompressed_data.reserve(batch.size() * 128);

    for (const auto& log : batch) {
        min_ts = std::min(min_ts, log.timestamp);
        max_ts = std::max(max_ts, log.timestamp);
        services.insert(log.service);
        levels.insert(log.level);
        service_counts[log.service] += log.repeat_count;
        level_counts[log.level] += log.repeat_count;
        bloom.add_token(log.service);
        if (!log.trace_id.empty()) {
            bloom.add_token(log.trace_id);
        }
        bloom.add_text(log.message);
        if (!log.raw_json.empty()) {
            bloom.add_text(log.raw_json);
        }

        nlohmann::json item = log.to_json();
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
    fs::path chunk_tmp_path = target_dir / (chunk_id + ".zst.tmp");
    fs::path meta_path = target_dir / (chunk_id + ".meta");
    fs::path meta_tmp_path = target_dir / (chunk_id + ".meta.tmp");

    {
        std::ofstream zst_file(chunk_tmp_path, std::ios::binary);
        if (!zst_file) {
            std::cerr << "[storage] failed to open chunk file for writing: " << chunk_tmp_path << std::endl;
            return;
        }
        zst_file.write(compressed_data.data(), static_cast<std::streamsize>(compressed_size));
        zst_file.flush();
        if (!zst_file) {
            std::cerr << "[storage] failed to write chunk data: " << chunk_tmp_path << std::endl;
            return;
        }
    }
    {
        std::error_code ec;
        fs::rename(chunk_tmp_path, chunk_path, ec);
        if (ec) {
            std::cerr << "[storage] failed to commit chunk file: " << ec.message() << std::endl;
            return;
        }
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
    meta.service_counts = std::move(service_counts);
    meta.level_counts = std::move(level_counts);
    meta.uncompressed_bytes = uncompressed_size;
    meta.compressed_bytes = compressed_size;
    meta.has_bloom = true;
    meta.bloom = std::move(bloom);

    {
        std::ofstream meta_file(meta_tmp_path);
        if (meta_file) {
            meta_file << meta.to_json().dump(2);
            meta_file.flush();
        }
    }
    {
        std::error_code ec;
        fs::rename(meta_tmp_path, meta_path, ec);
    }

    if (!wal_flushing_path.empty()) {
        std::error_code ec;
        fs::remove(wal_flushing_path, ec);
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

void LogStorage::recover_wal_file(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return;

    std::ifstream in(path);
    if (!in.is_open()) return;

    std::vector<LogEntry> recovered;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            nlohmann::json j = nlohmann::json::parse(line);
            recovered.push_back(LogEntry::from_json(j));
        } catch (...) {
            // Skip partial trailing line from an abrupt crash
        }
    }
    in.close();

    if (!recovered.empty()) {
        std::cout << "[wal] recovering " << recovered.size() << " uncommitted logs from " << path << std::endl;
        total_logs_ingested_.fetch_add(recovered.size(), std::memory_order_relaxed);
        flush_batch_to_disk(recovered, path);
    } else {
        fs::remove(path, ec);
    }
}

void LogStorage::load_existing_chunks() {
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        chunks_.clear();
        known_services_.clear();
        total_compressed_bytes_ = 0;
        total_uncompressed_bytes_ = 0;

        if (!fs::exists(data_dir_)) return;

        for (const auto& entry : fs::recursive_directory_iterator(data_dir_)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() == ".tmp") {
                std::error_code ec;
                fs::remove(entry.path(), ec);
                continue;
            }
            if (entry.path().extension() == ".meta") {
                try {
                    std::ifstream f(entry.path());
                    if (!f) continue;
                    nlohmann::json j = nlohmann::json::parse(f);
                    ChunkMeta meta = ChunkMeta::from_json(j, entry.path().string());

                    if (!fs::exists(meta.chunk_file_path)) {
                        fs::path alt = entry.path().parent_path() / (meta.id + ".zst");
                        if (fs::exists(alt)) {
                            meta.chunk_file_path = alt.string();
                        } else {
                            continue;
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

    // Replay any uncommitted WAL files left from an abrupt shutdown
    recover_wal_file(wal_flushing_path_);
    recover_wal_file(wal_path_);
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
            LogEntry entry = LogEntry::from_json(j);

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
    std::vector<std::string> req_terms;
    if (!params.query.empty()) {
        parser = std::make_unique<LogSqlParser>(params.query);
        req_terms = parser->required_terms();
    }

    std::vector<ChunkMeta> candidate_chunks;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        result.total_scanned_chunks = chunks_.size();
        for (const auto& ch : chunks_) {
            if (!ch.can_skip(params.from_ts, params.to_ts, params.service, params.level) &&
                !ch.can_skip_terms(req_terms)) {
                candidate_chunks.push_back(ch);
            }
        }
    }
    std::sort(candidate_chunks.begin(), candidate_chunks.end(), [](const ChunkMeta& a, const ChunkMeta& b) {
        return a.max_ts > b.max_ts;
    });
    result.matched_chunks = candidate_chunks.size();

    std::vector<LogEntry> all_matches;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& entry : current_batch_) {
            if (matches_filter(entry, params, parser.get())) {
                all_matches.push_back(entry);
            }
        }
    }

    auto can_early_exit = [&](size_t next_chunk_idx) -> bool {
        if (next_chunk_idx >= candidate_chunks.size()) return true;
        if (all_matches.size() < params.limit) return false;
        std::nth_element(all_matches.begin(),
                         all_matches.begin() + static_cast<ptrdiff_t>(params.limit - 1),
                         all_matches.end(),
                         [](const LogEntry& a, const LogEntry& b) { return a.timestamp > b.timestamp; });
        int64_t kth_ts = all_matches[params.limit - 1].timestamp;
        return candidate_chunks[next_chunk_idx].max_ts < kth_ts;
    };

    // Wave-based bounded parallel scan with Top-N early exit (newest chunks first)
    size_t hw = std::thread::hardware_concurrency();
    size_t wave_size = std::min<size_t>(std::max<size_t>(hw, 2), 8);
    size_t cursor = 0;

    while (cursor < candidate_chunks.size()) {
        if (can_early_exit(cursor)) {
            break;
        }
        size_t end_idx = std::min(candidate_chunks.size(), cursor + wave_size);
        size_t count = end_idx - cursor;

        if (count == 1) {
            auto chunk_matches = search_chunk_file(candidate_chunks[cursor], params, parser.get());
            all_matches.insert(all_matches.end(),
                               std::make_move_iterator(chunk_matches.begin()),
                               std::make_move_iterator(chunk_matches.end()));
        } else {
            std::vector<std::vector<LogEntry>> wave_results(count);
            std::vector<std::thread> workers;
            workers.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                workers.emplace_back([&, i, idx = cursor + i]() {
                    wave_results[i] = search_chunk_file(candidate_chunks[idx], params, parser.get());
                });
            }
            for (auto& t : workers) t.join();
            for (auto& wr : wave_results) {
                all_matches.insert(all_matches.end(),
                                   std::make_move_iterator(wr.begin()),
                                   std::make_move_iterator(wr.end()));
            }
        }
        cursor = end_idx;
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
    std::vector<std::string> req_terms;
    if (!params.query.empty()) {
        parser = std::make_unique<LogSqlParser>(params.query);
        req_terms = parser->required_terms();
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
            if (!ch.can_skip(from_ts, to_ts, params.service, params.level) &&
                !ch.can_skip_terms(req_terms)) {
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

AnalyticsResult LogStorage::get_analytics(int64_t from_ts, int64_t to_ts) {
    auto start_time = std::chrono::steady_clock::now();
    AnalyticsResult res;
    std::vector<ChunkMeta> error_chunks;

    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        for (const auto& ch : chunks_) {
            if (from_ts > 0 && ch.max_ts < from_ts) continue;
            if (to_ts > 0 && ch.min_ts > to_ts) continue;

            if (!ch.service_counts.empty()) {
                for (const auto& [svc, cnt] : ch.service_counts) {
                    res.service_totals[svc] += cnt;
                }
            } else if (!ch.services.empty()) {
                size_t per_svc = std::max<size_t>(1, ch.count / ch.services.size());
                for (const auto& svc : ch.services) {
                    res.service_totals[svc] += per_svc;
                }
            }

            if (!ch.level_counts.empty()) {
                for (const auto& [lvl, cnt] : ch.level_counts) {
                    res.level_totals[lvl] += cnt;
                    res.total_logs += cnt;
                    if (lvl == "ERROR" || lvl == "FATAL") res.total_errors += cnt;
                    else if (lvl == "WARN") res.total_warns += cnt;
                }
            } else {
                res.total_logs += ch.count;
            }

            if (ch.levels.count("ERROR") || ch.levels.count("FATAL")) {
                error_chunks.push_back(ch);
            }
        }
    }

    std::unordered_map<std::string, size_t> err_msg_counts;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& e : current_batch_) {
            if (from_ts > 0 && e.timestamp < from_ts) continue;
            if (to_ts > 0 && e.timestamp > to_ts) continue;
            res.service_totals[e.service] += e.repeat_count;
            res.level_totals[e.level] += e.repeat_count;
            res.total_logs += e.repeat_count;
            if (e.level == "ERROR" || e.level == "FATAL") {
                res.total_errors += e.repeat_count;
                err_msg_counts[e.service + ": " + e.message] += e.repeat_count;
            } else if (e.level == "WARN") {
                res.total_warns += e.repeat_count;
            }
        }
    }

    // Sample up to 8 most recent error chunks for top error messages without scanning the full archive
    std::sort(error_chunks.begin(), error_chunks.end(), [](const ChunkMeta& a, const ChunkMeta& b) {
        return a.max_ts > b.max_ts;
    });
    if (error_chunks.size() > 8) error_chunks.resize(8);

    SearchParams err_params;
    err_params.from_ts = from_ts;
    err_params.to_ts = to_ts;
    err_params.level = "ERROR";
    for (const auto& ch : error_chunks) {
        for (const auto& log : search_chunk_file(ch, err_params, nullptr)) {
            err_msg_counts[log.service + ": " + log.message] += log.repeat_count;
        }
    }

    res.top_errors.assign(err_msg_counts.begin(), err_msg_counts.end());
    std::sort(res.top_errors.begin(), res.top_errors.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    if (res.top_errors.size() > 10) res.top_errors.resize(10);

    auto end_time = std::chrono::steady_clock::now();
    res.duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    return res;
}

std::vector<LogEntry> LogStorage::get_trace(const std::string& trace_id) {
    if (trace_id.empty()) return {};

    std::vector<LogEntry> matched;

    // 1. Scan in-memory batch
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        for (const auto& log : current_batch_) {
            if (log.trace_id == trace_id ||
                log.message.find(trace_id) != std::string::npos ||
                log.raw_json.find(trace_id) != std::string::npos) {
                matched.push_back(log);
            }
        }
    }

    // 2. Scan chunk files on disk
    std::vector<ChunkMeta> chunks_copy;
    {
        std::lock_guard<std::mutex> lock(metadata_mutex_);
        chunks_copy = chunks_;
    }

    for (const auto& chunk : chunks_copy) {
        if (chunk.has_bloom && !chunk.bloom.might_contain_text(trace_id)) {
            continue;
        }
        std::ifstream file(chunk.chunk_file_path, std::ios::binary);
        if (!file.is_open()) continue;

        std::vector<char> compressed(chunk.compressed_bytes);
        file.read(compressed.data(), chunk.compressed_bytes);

        std::string uncompressed(chunk.uncompressed_bytes, '\0');
        size_t ret = ZSTD_decompress(&uncompressed[0], chunk.uncompressed_bytes,
                                     compressed.data(), chunk.compressed_bytes);
        if (ZSTD_isError(ret)) continue;

        // Fast rejection: check if trace_id exists in uncompressed chunk string
        if (uncompressed.find(trace_id) == std::string::npos) {
            continue;
        }

        std::stringstream ss(uncompressed);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            try {
                auto j = nlohmann::json::parse(line);
                LogEntry entry = LogEntry::from_json(j);
                if (entry.trace_id == trace_id ||
                    entry.message.find(trace_id) != std::string::npos ||
                    entry.raw_json.find(trace_id) != std::string::npos) {
                    matched.push_back(std::move(entry));
                }
            } catch (...) {}
        }
    }

    // Sort chronologically (earliest span first)
    std::sort(matched.begin(), matched.end(), [](const LogEntry& a, const LogEntry& b) {
        return a.timestamp < b.timestamp;
    });

    return matched;
}
