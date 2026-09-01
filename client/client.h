#pragma once

#include "json_util.h"
#include "protocol.h"

#include <cstdint>
#include <functional>
#include <string>

namespace cloud {

using ProgressCallback = std::function<void(uint64_t current, uint64_t total, const std::string& phase)>;

class CloudClient {
public:
    CloudClient() = default;
    ~CloudClient();

    void connectTo(const std::string& host, uint16_t port);
    void close();
    bool connected() const { return fd_ >= 0; }

    void setProgressCallback(ProgressCallback cb) { progress_ = std::move(cb); }

    Json::Value request(const std::string& cmd, const Json::Value& data = Json::Value());
    Json::Value login(const std::string& user, const std::string& pass);
    Json::Value signup(const std::string& user, const std::string& pass);
    void logout();

    Json::Value listEntries(const std::string& path, bool recursive = false);
    Json::Value makeDir(const std::string& path);
    Json::Value removePath(const std::string& path);
    Json::Value statPath(const std::string& path);
    Json::Value renamePath(const std::string& from, const std::string& to);

    Json::Value put(const std::string& localPath, const std::string& remotePath, bool overwrite = false);
    Json::Value get(const std::string& remotePath, const std::string& localPath);

    // CLI helpers (print to stdout)
    void ls(const std::string& path, bool recursive = false);
    void mkdir(const std::string& path);
    void rm(const std::string& path);
    void stat(const std::string& path);
    void rename(const std::string& from, const std::string& to);

    bool loggedIn() const { return !token_.empty(); }
    const std::string& username() const { return username_; }
    const std::string& host() const { return host_; }
    uint16_t port() const { return port_; }

private:
    void reportProgress(uint64_t current, uint64_t total, const std::string& phase);
    void sendFrame(FrameType type, const std::string& payload);
    Frame recvFrame();
    Json::Value recvJson();
    void sendChunk(const std::string& fileId, uint64_t offset, const void* data, uint32_t len);
    void writeAll(const void* data, size_t len);
    void readAll(void* data, size_t len);

    int fd_ = -1;
    uint64_t seq_ = 0;
    std::string token_;
    std::string username_;
    std::string host_;
    uint16_t port_ = 0;
    ProgressCallback progress_;
};

}  // namespace cloud
