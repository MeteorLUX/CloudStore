#include "client.h"
#include "crypto_util.h"
#include "logger.h"
#include "utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace cloud {

CloudClient::~CloudClient() { close(); }

void CloudClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void CloudClient::writeAll(const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t left = len;
    while (left) {
        ssize_t n = ::send(fd_, p, left, MSG_NOSIGNAL);
        if (n <= 0) {
            throw std::runtime_error("connection closed while sending");
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
}

void CloudClient::readAll(void* data, size_t len) {
    char* p = static_cast<char*>(data);
    size_t left = len;
    while (left) {
        ssize_t n = ::recv(fd_, p, left, 0);
        if (n <= 0) {
            throw std::runtime_error("connection closed while receiving");
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
}

void CloudClient::connectTo(const std::string& host, uint16_t port) {
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("socket failed");
    }
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("invalid host ip (use IPv4): " + host);
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("connect failed: " + host + ":" + std::to_string(port));
    }
    host_ = host;
    port_ = port;
}

void CloudClient::sendFrame(FrameType type, const std::string& payload) {
    auto raw = Protocol::encode(type, payload);
    writeAll(raw.data(), raw.size());
}

Frame CloudClient::recvFrame() {
    uint8_t hdr[Protocol::kHeaderSize];
    readAll(hdr, sizeof(hdr));
    uint8_t ver = 0;
    FrameType type{};
    uint16_t flags = 0;
    uint32_t len = 0;
    if (!Protocol::parseHeader(hdr, ver, type, flags, len)) {
        throw std::runtime_error("bad server frame");
    }
    Frame f;
    f.type = type;
    f.flags = flags;
    f.payload.resize(len);
    if (len) {
        readAll(&f.payload[0], len);
    }
    return f;
}

Json::Value CloudClient::recvJson() {
    auto f = recvFrame();
    if (f.type != FrameType::Json) {
        throw std::runtime_error("expected json reply");
    }
    Json::Value v;
    if (!parseJson(f.payload, v)) {
        throw std::runtime_error("bad json reply");
    }
    return v;
}

void CloudClient::sendChunk(const std::string& fileId, uint64_t offset, const void* data,
                            uint32_t len) {
    auto raw = Protocol::encodeChunk(fileId, offset, data, len);
    writeAll(raw.data(), raw.size());
}

Json::Value CloudClient::request(const std::string& cmd, const Json::Value& data) {
    Json::Value root;
    root["cmd"] = cmd;
    root["seq"] = static_cast<Json::UInt64>(++seq_);
    if (!token_.empty()) {
        root["token"] = token_;
    }
    root["data"] = data;
    sendFrame(FrameType::Json, dumpJson(root));
    auto reply = recvJson();
    int code = reply.get("code", 500).asInt();
    if (code != 0) {
        throw std::runtime_error(cmd + " failed: " + reply.get("msg", "error").asString());
    }
    return reply;
}

void CloudClient::login(const std::string& user, const std::string& pass) {
    Json::Value d;
    d["username"] = user;
    d["password"] = pass;
    auto r = request("login", d);
    token_ = r["data"]["token"].asString();
    username_ = r["data"]["username"].asString();
    std::cout << "login ok, user=" << username_ << "\n";
}

void CloudClient::signup(const std::string& user, const std::string& pass) {
    Json::Value d;
    d["username"] = user;
    d["password"] = pass;
    request("register", d);
    std::cout << "register ok, please login\n";
}

void CloudClient::logout() {
    request("logout");
    token_.clear();
    username_.clear();
    std::cout << "logout ok\n";
}

void CloudClient::ls(const std::string& path, bool recursive) {
    Json::Value d;
    d["path"] = path;
    d["recursive"] = recursive;
    auto r = request("ls", d);
    const auto& arr = r["data"]["entries"];
    std::cout << "path: " << r["data"]["path"].asString() << "\n";
    for (const auto& e : arr) {
        std::cout << "  " << e.get("type", "").asString() << "\t"
                  << e.get("size", 0).asUInt64() << "\t" << e.get("status", "").asString() << "\t"
                  << e.get("path", e.get("name", "")).asString() << "\n";
    }
}

void CloudClient::mkdir(const std::string& path) {
    Json::Value d;
    d["path"] = path;
    request("mkdir", d);
    std::cout << "mkdir ok\n";
}

void CloudClient::rm(const std::string& path) {
    Json::Value d;
    d["path"] = path;
    request("rm", d);
    std::cout << "rm ok\n";
}

void CloudClient::stat(const std::string& path) {
    Json::Value d;
    d["path"] = path;
    auto r = request("stat", d);
    std::cout << dumpJson(r["data"]) << "\n";
}

void CloudClient::rename(const std::string& from, const std::string& to) {
    Json::Value d;
    d["from"] = from;
    d["to"] = to;
    request("rename", d);
    std::cout << "rename ok\n";
}

void CloudClient::put(const std::string& localPath, const std::string& remotePath, bool overwrite) {
    if (!fileExists(localPath)) {
        throw std::runtime_error("local file not found: " + localPath);
    }
    uint64_t size = fileSize(localPath);
    std::cout << "reading local file to compute md5 ...\n";
    std::string md5 = md5File(localPath);
    std::cout << "md5=" << md5 << " size=" << size << "\n";

    Json::Value q;
    q["md5"] = md5;
    q["size"] = static_cast<Json::UInt64>(size);
    auto qr = request("instant_query", q);
    bool hit = qr["data"].get("hit", false).asBool();
    if (hit) {
        uint64_t off = qr["data"]["challenge_offset"].asUInt64();
        uint32_t len = qr["data"]["challenge_len"].asUInt();
        std::string proof = (len == 0) ? md5 : md5FileRange(localPath, off, len);
        Json::Value up;
        up["path"] = remotePath;
        up["md5"] = md5;
        up["size"] = static_cast<Json::UInt64>(size);
        up["challenge_offset"] = static_cast<Json::UInt64>(off);
        up["proof_md5"] = proof;
        up["overwrite"] = overwrite;
        auto ir = request("instant_upload", up);
        std::cout << "秒传成功：文件已可立即下载；服务端正在后台把独立副本写入你的目录\n";
        std::cout << dumpJson(ir["data"]) << "\n";
        return;
    }

    Json::Value b;
    b["path"] = remotePath;
    b["md5"] = md5;
    b["size"] = static_cast<Json::UInt64>(size);
    auto br = request("upload_begin", b);
    std::string fileId = br["data"]["file_id"].asString();
    uint64_t offset = br["data"]["offset"].asUInt64();
    uint32_t chunk = br["data"].get("chunk_size", 65536).asUInt();
    if (offset) {
        std::cout << "resume from offset " << offset << "\n";
    }

    std::ifstream in(localPath, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    std::vector<char> buf(chunk);
    uint64_t sent = offset;
    while (sent < size) {
        uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(chunk, size - sent));
        in.read(buf.data(), want);
        auto n = static_cast<uint32_t>(in.gcount());
        if (n == 0) {
            break;
        }
        sendChunk(fileId, sent, buf.data(), n);
        sent += n;
        if (sent % (chunk * 32) == 0 || sent == size) {
            std::cout << "\rupload " << sent << "/" << size << std::flush;
        }
    }
    std::cout << "\n";
    request("upload_end");
    std::cout << "upload complete, md5 verified\n";
}

void CloudClient::get(const std::string& remotePath, const std::string& localPath) {
    Json::Value d;
    d["path"] = remotePath;
    auto br = request("download_begin", d);
    std::string fileId = br["data"]["file_id"].asString();
    uint64_t size = br["data"]["size"].asUInt64();
    uint32_t chunk = br["data"].get("chunk_size", 65536).asUInt();
    ensureDir(parentDir(localPath));
    std::ofstream out(localPath, std::ios::binary | std::ios::trunc);
    uint64_t got = 0;
    while (got < size) {
        Json::Value req;
        req["file_id"] = fileId;
        req["offset"] = static_cast<Json::UInt64>(got);
        req["length"] = chunk;
        Json::Value root;
        root["cmd"] = "download_chunk";
        root["seq"] = static_cast<Json::UInt64>(++seq_);
        root["token"] = token_;
        root["data"] = req;
        sendFrame(FrameType::Json, dumpJson(root));
        auto f = recvFrame();
        if (f.type == FrameType::Json) {
            Json::Value v;
            parseJson(f.payload, v);
            if (v.get("code", 1).asInt() != 0) {
                throw std::runtime_error(v.get("msg", "download failed").asString());
            }
            if (v["data"].get("eof", false).asBool()) {
                break;
            }
            continue;
        }
        ChunkPayload cp;
        if (!Protocol::decodeChunk(f.payload, cp)) {
            throw std::runtime_error("bad download chunk");
        }
        out.write(cp.data.data(), static_cast<std::streamsize>(cp.data.size()));
        got += cp.data.size();
        std::cout << "\rdownload " << got << "/" << size << std::flush;
    }
    std::cout << "\nsaved to " << localPath << "\n";
}

}  // namespace cloud
