#include "enterprise/audit_ledger.hpp"
#include "storage/log_entry.hpp"
#include "utils/sha256.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

AuditLedger::AuditLedger(std::string log_file_path)
    : log_file_path_(std::move(log_file_path)),
      last_hash_("0000000000000000000000000000000000000000000000000000000000000000") {
    if (!log_file_path_.empty()) {
        fs::path p(log_file_path_);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
        }

        // Recover last hash from existing audit trail if present
        std::ifstream in(log_file_path_);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                try {
                    auto j = nlohmann::json::parse(line);
                    if (j.contains("hash") && j["hash"].is_string()) {
                        last_hash_ = j["hash"].get<std::string>();
                    }
                } catch (...) {}
            }
        }
    }
}

void AuditLedger::record_event(const std::string& actor,
                               const std::string& action,
                               const std::string& details,
                               const std::string& status) {
    AuditRecord rec;
    rec.timestamp = LogEntry::now_ms();
    rec.actor = actor.empty() ? "anonymous" : actor;
    rec.action = action;
    rec.details = details;
    rec.status = status;

    std::lock_guard<std::mutex> lock(mutex_);
    rec.prev_hash = last_hash_;
    std::string payload = rec.prev_hash + "|" +
                          std::to_string(rec.timestamp) + "|" +
                          rec.actor + "|" +
                          rec.action + "|" +
                          rec.details + "|" +
                          rec.status;
    rec.hash = siftlog::crypto::sha256_hex(payload);
    last_hash_ = rec.hash;

    memory_cache_.push_back(rec);
    if (memory_cache_.size() > 500) {
        memory_cache_.erase(memory_cache_.begin(), memory_cache_.begin() + 100);
    }

    if (!log_file_path_.empty()) {
        std::ofstream out(log_file_path_, std::ios::app);
        if (out.is_open()) {
            out << rec.to_json().dump() << "\n";
        }
    }
}

std::vector<AuditRecord> AuditLedger::get_recent_events(size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (memory_cache_.size() <= limit) {
        return memory_cache_;
    }
    return std::vector<AuditRecord>(memory_cache_.end() - static_cast<ptrdiff_t>(limit), memory_cache_.end());
}
