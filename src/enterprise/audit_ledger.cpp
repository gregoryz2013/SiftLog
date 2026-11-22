#include "enterprise/audit_ledger.hpp"
#include "storage/log_entry.hpp"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

AuditLedger::AuditLedger(std::string log_file_path)
    : log_file_path_(std::move(log_file_path)) {
    if (!log_file_path_.empty()) {
        fs::path p(log_file_path_);
        if (p.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
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
