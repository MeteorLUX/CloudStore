#include "connection.h"
#include "logger.h"
#include "path_guard.h"
#include "server.h"
#include "utils.h"

#include <event2/buffer.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace cloud {

Connection::Connection(Server* server, bufferevent* bev, const std::string& peerIp)
    : server_(server), bev_(bev), peerIp_(peerIp) {
    bufferevent_setcb(bev_, &Connection::readCb, nullptr, &Connection::eventCb, this);
    bufferevent_setwatermark(bev_, EV_READ, 0, kMaxFrameSize + Protocol::kHeaderSize);
    timeval tv{};
    tv.tv_sec = server_->config().connIdleTimeoutSec;
    bufferevent_set_timeouts(bev_, &tv, &tv);
    bufferevent_enable(bev_, EV_READ | EV_WRITE);
}

Connection::~Connection() {
    server_->transfer().closeUpload(upload_);
    server_->transfer().closeDownload(download_);
    if (bev_) {
        bufferevent_free(bev_);
        bev_ = nullptr;
    }
}

void Connection::readCb(bufferevent*, void* ctx) {
    static_cast<Connection*>(ctx)->onRead();
}

void Connection::eventCb(bufferevent*, short what, void* ctx) {
    static_cast<Connection*>(ctx)->onEvent(what);
}

void Connection::close() {
    if (bev_) {
        bufferevent_disable(bev_, EV_READ | EV_WRITE);
    }
    server_->removeConnection(this);
}

void Connection::onEvent(short what) {
    if (what & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)) {
        logInfo("connection closed " + peerIp_);
        close();
    }
}

void Connection::onRead() {
    evbuffer* input = bufferevent_get_input(bev_);
    while (true) {
        size_t avail = evbuffer_get_length(input);
        if (avail < Protocol::kHeaderSize) {
            return;
        }
        uint8_t hdr[Protocol::kHeaderSize];
        evbuffer_copyout(input, hdr, Protocol::kHeaderSize);
        uint8_t ver = 0;
        FrameType type{};
        uint16_t flags = 0;
        uint32_t len = 0;
        if (!Protocol::parseHeader(hdr, ver, type, flags, len)) {
            logWarn("bad frame from " + peerIp_);
            close();
            return;
        }
        if (avail < Protocol::kHeaderSize + len) {
            return;
        }
        evbuffer_drain(input, Protocol::kHeaderSize);
        Frame frame;
        frame.type = type;
        frame.flags = flags;
        frame.payload.resize(len);
        if (len) {
            evbuffer_remove(input, &frame.payload[0], len);
        }
        try {
            handleFrame(frame);
        } catch (const CloudError& e) {
            sendError(0, e.code(), e.what());
        } catch (const std::exception& e) {
            sendError(0, ErrorCode::Internal, e.what());
        }
    }
}

void Connection::handleFrame(const Frame& frame) {
    if (frame.type == FrameType::Json) {
        handleJson(frame.payload);
        return;
    }
    if (frame.type == FrameType::Chunk) {
        ChunkPayload chunk;
        if (!Protocol::decodeChunk(frame.payload, chunk)) {
            throw CloudError(ErrorCode::BadRequest, "bad chunk");
        }
        handleChunk(chunk);
        return;
    }
    throw CloudError(ErrorCode::BadRequest, "unknown frame type");
}

void Connection::sendJson(const Json::Value& v) {
    auto raw = Protocol::encode(FrameType::Json, dumpJson(v));
    bufferevent_write(bev_, raw.data(), raw.size());
}

void Connection::sendError(uint64_t seq, ErrorCode code, const std::string& msg) {
    sendJson(makeReply(seq, static_cast<int>(code), msg));
}

void Connection::sendChunk(const std::string& fileId, uint64_t offset, const void* data,
                           uint32_t len) {
    auto raw = Protocol::encodeChunk(fileId, offset, data, len);
    bufferevent_write(bev_, raw.data(), raw.size());
}

void Connection::requireLogin(uint64_t seq) {
    (void)seq;
    if (!authed_) {
        throw CloudError(ErrorCode::Unauthorized, "login required");
    }
}

std::string Connection::resolveUserPath(const std::string& logical, bool mustExist) {
    requireLogin(0);
    return PathGuard::resolve(session_.rootDir, logical, mustExist);
}

void Connection::handleJson(const std::string& payload) {
    Json::Value root;
    std::string err;
    if (!parseJson(payload, root, &err)) {
        sendError(0, ErrorCode::BadRequest, "bad json: " + err);
        return;
    }
    std::string cmd = root.get("cmd", "").asString();
    uint64_t seq = root.get("seq", 0).asUInt64();
    if (!authed_ && root.isMember("token") && !root["token"].asString().empty() &&
        cmd != "login" && cmd != "register") {
        session_ = server_->auth().checkToken(root["token"].asString());
        authed_ = true;
    }
    try {
        if (cmd == "register") {
            onRegister(root, seq);
        } else if (cmd == "login") {
            onLogin(root, seq);
        } else if (cmd == "logout") {
            onLogout(root, seq);
        } else if (cmd == "ls") {
            onList(root, seq);
        } else if (cmd == "mkdir") {
            onMkdir(root, seq);
        } else if (cmd == "rm") {
            onRm(root, seq);
        } else if (cmd == "stat") {
            onStat(root, seq);
        } else if (cmd == "rename" || cmd == "mv") {
            onRename(root, seq);
        } else if (cmd == "instant_query") {
            onInstantQuery(root, seq);
        } else if (cmd == "instant_upload") {
            onInstantUpload(root, seq);
        } else if (cmd == "upload_begin") {
            onUploadBegin(root, seq);
        } else if (cmd == "upload_end") {
            onUploadEnd(root, seq);
        } else if (cmd == "download_begin") {
            onDownloadBegin(root, seq);
        } else if (cmd == "download_chunk") {
            onDownloadChunk(root, seq);
        } else {
            sendError(seq, ErrorCode::BadRequest, "unknown cmd: " + cmd);
        }
    } catch (const CloudError& e) {
        sendError(seq, e.code(), e.what());
    }
}

void Connection::handleChunk(const ChunkPayload& chunk) {
    if (!authed_) {
        throw CloudError(ErrorCode::Unauthorized, "login required");
    }
    if (upload_.fileId.empty() || chunk.fileId != upload_.fileId) {
        throw CloudError(ErrorCode::BadRequest, "unexpected upload chunk");
    }
    server_->transfer().writeChunk(upload_, chunk.offset, chunk.data.data(),
                                   static_cast<uint32_t>(chunk.data.size()));
}

void Connection::onRegister(const Json::Value& root, uint64_t seq) {
    auto data = root["data"];
    auto s = server_->auth().registerUser(data.get("username", "").asString(),
                                          data.get("password", "").asString());
    Json::Value out;
    out["username"] = s.username;
    out["user_id"] = s.userId;
    sendJson(makeReply(seq, 0, "ok", out));
}

void Connection::onLogin(const Json::Value& root, uint64_t seq) {
    auto data = root["data"];
    session_ = server_->auth().login(data.get("username", "").asString(),
                                     data.get("password", "").asString(), peerIp_);
    authed_ = true;
    Json::Value out;
    out["token"] = session_.token;
    out["username"] = session_.username;
    out["user_id"] = session_.userId;
    out["root"] = "/";
    sendJson(makeReply(seq, 0, "ok", out));
}

void Connection::onLogout(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    (void)root;
    server_->auth().logout(session_.token);
    authed_ = false;
    session_ = UserSession{};
    sendJson(makeReply(seq, 0, "ok"));
}

void Connection::onList(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    std::string path = root["data"].get("path", "/").asString();
    bool recursive = root["data"].get("recursive", false).asBool();
    std::string logical = PathGuard::normalizeLogical(path);
    std::string dir = logical.empty() ? "/" : ("/" + logical);

    Json::Value arr(Json::arrayValue);
    std::unordered_map<std::string, int> seen;
    try {
        std::string phys = resolveUserPath(path, true);
        auto entries = FileManager::listDir(phys, recursive);
        for (const auto& e : entries) {
            Json::Value item;
            item["name"] = e.name;
            item["path"] = e.path;
            item["type"] = e.type;
            item["size"] = static_cast<Json::UInt64>(e.size);
            item["mtime"] = static_cast<Json::Int64>(e.mtime);
            item["status"] = e.type == "file" ? "ready" : "ok";
            arr.append(item);
            seen[e.name] = 1;
        }
    } catch (const CloudError& e) {
        if (e.code() != ErrorCode::NotFound) {
            throw;
        }
    }

    auto indexed = server_->transfer().listIndexed(session_.userId, dir);
    for (const auto& f : indexed) {
        std::string name = baseName(f.virtualPath);
        if (name.empty() || seen[name]) {
            continue;
        }
        Json::Value item;
        item["name"] = name;
        item["path"] = f.virtualPath;
        item["type"] = "file";
        item["size"] = static_cast<Json::UInt64>(f.size);
        item["mtime"] = 0;
        item["status"] = f.status.empty() ? "ready" : f.status;
        item["md5"] = f.md5;
        arr.append(item);
    }
    Json::Value data;
    data["path"] = dir;
    data["entries"] = arr;
    sendJson(makeReply(seq, 0, "ok", data));
}

void Connection::onMkdir(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    std::string path = root["data"].get("path", "").asString();
    std::string phys = resolveUserPath(path, false);
    FileManager::makeDir(phys);
    std::string logical = "/" + PathGuard::normalizeLogical(path);
    server_->transfer().indexFile(session_.userId, logical, "", 0, true, "ready");
    sendJson(makeReply(seq, 0, "ok"));
}

void Connection::onRm(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    std::string path = root["data"].get("path", "").asString();
    std::string logical = PathGuard::normalizeLogical(path);
    if (logical.empty()) {
        throw CloudError(ErrorCode::Forbidden, "cannot remove user root");
    }
    std::string phys = resolveUserPath(path, false);
    server_->transfer().removeUserPath(session_, phys, "/" + logical);
    sendJson(makeReply(seq, 0, "ok"));
}

void Connection::onStat(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    std::string path = root["data"].get("path", "/").asString();
    std::string logical = PathGuard::normalizeLogical(path);
    std::string vpath = logical.empty() ? "/" : ("/" + logical);
    Json::Value data;

    IndexedFile indexed;
    if (server_->transfer().lookupFile(session_.userId, vpath, indexed) && !indexed.isDir) {
        data["name"] = baseName(vpath);
        data["type"] = "file";
        data["size"] = static_cast<Json::UInt64>(indexed.size);
        data["mtime"] = 0;
        data["status"] = indexed.status.empty() ? "ready" : indexed.status;
        data["md5"] = indexed.md5;
        data["readable"] = true;
        sendJson(makeReply(seq, 0, "ok", data));
        return;
    }

    auto e = FileManager::statPath(resolveUserPath(path, true));
    data["name"] = e.name;
    data["type"] = e.type;
    data["size"] = static_cast<Json::UInt64>(e.size);
    data["mtime"] = static_cast<Json::Int64>(e.mtime);
    data["status"] = e.type == "file" ? "ready" : "ok";
    sendJson(makeReply(seq, 0, "ok", data));
}

void Connection::onRename(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    std::string from = root["data"].get("from", "").asString();
    std::string to = root["data"].get("to", "").asString();
    auto fromL = PathGuard::normalizeLogical(from);
    auto toL = PathGuard::normalizeLogical(to);
    std::string fromVirtual = "/" + fromL;
    std::string toVirtual = "/" + toL;

    std::string fromPhys;
    try {
        fromPhys = resolveUserPath(from, true);
    } catch (const CloudError& e) {
        if (e.code() != ErrorCode::NotFound) {
            throw;
        }
        fromPhys = resolveUserPath(from, false);
    }
    server_->transfer().renameUserPath(session_, fromPhys, fromVirtual,
                                       resolveUserPath(to, false), toVirtual);
    sendJson(makeReply(seq, 0, "ok"));
}

void Connection::onInstantQuery(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    auto d = root["data"];
    auto q = server_->transfer().queryInstant(session_, d.get("md5", "").asString(),
                                              d.get("size", 0).asUInt64());
    Json::Value data;
    data["hit"] = q.hit;
    data["size"] = static_cast<Json::UInt64>(q.size);
    data["challenge_offset"] = static_cast<Json::UInt64>(q.challengeOffset);
    data["challenge_len"] = q.challengeLen;
    sendJson(makeReply(seq, 0, "ok", data));
}

void Connection::onInstantUpload(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    auto d = root["data"];
    std::string path = d.get("path", "").asString();
    std::string logical = PathGuard::normalizeLogical(path);
    std::string phys = resolveUserPath(path, false);
    server_->transfer().instantUpload(session_, phys, "/" + logical, d.get("md5", "").asString(),
                                      d.get("size", 0).asUInt64(),
                                      d.get("challenge_offset", 0).asUInt64(),
                                      d.get("proof_md5", "").asString(),
                                      d.get("overwrite", false).asBool());
    Json::Value data;
    data["instant"] = true;
    data["readable"] = true;
    data["status"] = "ready";
    data["msg"] = "instant upload via reference counting; content shared in object store";
    sendJson(makeReply(seq, 0, "ok", data));
}

void Connection::onUploadBegin(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    auto d = root["data"];
    std::string path = d.get("path", "").asString();
    std::string logical = PathGuard::normalizeLogical(path);
    server_->transfer().closeUpload(upload_);
    upload_ = server_->transfer().beginUpload(session_, resolveUserPath(path, false), "/" + logical,
                                              d.get("md5", "").asString(),
                                              d.get("size", 0).asUInt64());
    Json::Value data;
    data["file_id"] = upload_.fileId;
    data["offset"] = static_cast<Json::UInt64>(upload_.received);
    data["chunk_size"] = server_->config().chunkSize;
    sendJson(makeReply(seq, 0, "ok", data));
}

void Connection::onUploadEnd(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    (void)root;
    server_->transfer().finishUpload(session_, upload_);
    Json::Value data;
    data["md5"] = upload_.md5;
    data["size"] = static_cast<Json::UInt64>(upload_.totalSize);
    data["status"] = "ready";
    upload_ = UploadSession{};
    sendJson(makeReply(seq, 0, "ok", data));
}

void Connection::onDownloadBegin(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    std::string path = root["data"].get("path", "").asString();
    std::string logical = PathGuard::normalizeLogical(path);
    server_->transfer().closeDownload(download_);
    std::string phys;
    try {
        phys = resolveUserPath(path, true);
    } catch (const CloudError& e) {
        if (e.code() != ErrorCode::NotFound) {
            throw;
        }
        phys = resolveUserPath(path, false);
    }
    download_ = server_->transfer().beginDownload(session_, phys, "/" + logical);
    Json::Value data;
    data["file_id"] = download_.fileId;
    data["size"] = static_cast<Json::UInt64>(download_.totalSize);
    data["chunk_size"] = server_->config().chunkSize;
    sendJson(makeReply(seq, 0, "ok", data));
}

void Connection::onDownloadChunk(const Json::Value& root, uint64_t seq) {
    requireLogin(seq);
    auto d = root["data"];
    if (download_.fileId.empty() || d.get("file_id", "").asString() != download_.fileId) {
        throw CloudError(ErrorCode::BadRequest, "no such download");
    }
    uint64_t offset = d.get("offset", 0).asUInt64();
    uint32_t want = d.get("length", server_->config().chunkSize).asUInt();
    if (want == 0 || want > kMaxFrameSize - 64) {
        want = server_->config().chunkSize;
    }
    std::vector<char> buf(want);
    uint32_t n = server_->transfer().readChunk(download_, offset, buf.data(), want);
    if (n == 0) {
        Json::Value data;
        data["eof"] = true;
        data["offset"] = static_cast<Json::UInt64>(offset);
        sendJson(makeReply(seq, 0, "ok", data));
        return;
    }
    sendChunk(download_.fileId, offset, buf.data(), n);
}

}  // namespace cloud
