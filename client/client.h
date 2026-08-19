#pragma once

#include "json_util.h"
#include "protocol.h"

#include <cstdint>
#include <string>

namespace cloud {

class CloudClient {
public:
    CloudClient() = default;
    ~CloudClient();

    void connectTo(const std::string& host, uint16_t port);
    void close();

    Json::Value request(const std::string& cmd, const Json::Value& data = Json::Value());
    void login(const std::string& user, const std::string& pass);
    void signup(const std::string& user, const std::string& pass);
    void logout();

    void ls(const std::string& path, bool recursive = false);
    void mkdir(const std::string& path);
    void rm(const std::string& path);
    void stat(const std::string& path);
    void rename(const std::string& from, const std::string& to);

    // 先读本地文件算 MD5；命中则秒传（立刻可下载），否则分块上传（支持断点续传）
    void put(const std::string& localPath, const std::string& remotePath, bool overwrite = false);
    void get(const std::string& remotePath, const std::string& localPath);

    bool loggedIn() const { return !token_.empty(); }
    const std::string& username() const { return username_; }

private:
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
};

}  // namespace cloud
