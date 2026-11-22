#pragma once

#include <string>
#include <future>
#include <filesystem>

struct S3Config {
    bool enabled = false;
    std::string endpoint; // e.g. "storage.yandexcloud.net" or "s3.amazonaws.com"
    std::string bucket;   // e.g. "my-logs-bucket"
    std::string region = "us-east-1";
    std::string access_key;
    std::string secret_key;
    std::string prefix = "siftlog";
};

class S3Client {
public:
    explicit S3Client(S3Config config);

    bool is_enabled() const noexcept { return config_.enabled && !config_.bucket.empty(); }

    // Upload a local file to S3 synchronously
    bool upload_file(const std::string& local_file_path, const std::string& remote_key, std::string& error_out);

    // Upload a local file to S3 asynchronously in a background thread
    void upload_file_async(const std::string& local_file_path, const std::string& remote_key);

    const S3Config& config() const noexcept { return config_; }

private:
    S3Config config_;
};
