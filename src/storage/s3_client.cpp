#include "storage/s3_client.hpp"
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <thread>

S3Client::S3Client(S3Config config)
    : config_(std::move(config)) {
    if (config_.region.empty()) {
        config_.region = "us-east-1";
    }
    if (config_.endpoint.empty()) {
        config_.endpoint = "s3.amazonaws.com";
    }
}

bool S3Client::upload_file(const std::string& local_file_path, const std::string& remote_key, std::string& error_out) {
    if (!is_enabled()) {
        error_out = "S3 client is disabled or missing bucket name";
        return false;
    }

    std::string proto = "https://";
    std::string host = config_.endpoint;
    if (host.rfind("http://", 0) == 0) {
        proto = "";
    } else if (host.rfind("https://", 0) == 0) {
        proto = "";
    }

    std::string full_key = remote_key;
    if (!config_.prefix.empty()) {
        full_key = config_.prefix + "/" + remote_key;
    }

    std::string url = proto + host + "/" + config_.bucket + "/" + full_key;

    std::ostringstream cmd;
#if defined(_WIN32)
    cmd << "curl.exe -s -S -f -X PUT ";
#else
    cmd << "curl -s -S -f -X PUT ";
#endif

    if (!config_.access_key.empty() && !config_.secret_key.empty()) {
        cmd << "--aws-sigv4 \"aws:amz:" << config_.region << ":s3\" ";
        cmd << "--user \"" << config_.access_key << ":" << config_.secret_key << "\" ";
    }

    cmd << "-T \"" << local_file_path << "\" ";
    cmd << "\"" << url << "\" 2>&1";

    std::string command_str = cmd.str();
    FILE* pipe =
#if defined(_WIN32)
        _popen(command_str.c_str(), "r");
#else
        popen(command_str.c_str(), "r");
#endif

    if (!pipe) {
        error_out = "Failed to launch curl process for S3 upload";
        return false;
    }

    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    int ret =
#if defined(_WIN32)
        _pclose(pipe);
#else
        pclose(pipe);
#endif

    if (ret != 0) {
        error_out = "S3 upload failed (exit code " + std::to_string(ret) + "): " + output;
        return false;
    }

    return true;
}

void S3Client::upload_file_async(const std::string& local_file_path, const std::string& remote_key) {
    std::thread([this, local_file_path, remote_key]() {
        std::string err;
        if (upload_file(local_file_path, remote_key, err)) {
            std::cout << "[s3] uploaded " << local_file_path << " -> s3://"
                      << config_.bucket << "/" << config_.prefix << "/" << remote_key << std::endl;
        } else {
            std::cerr << "[s3] upload error for " << local_file_path << ": " << err << std::endl;
        }
    }).detach();
}
