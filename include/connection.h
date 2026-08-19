#pragma once

#include "auth.h"
#include "common.h"
#include "config.h"
#include "file_manager.h"
#include "json_util.h"
#include "protocol.h"
#include "redis_store.h"
#include "transfer.h"

#include <event2/bufferevent.h>
#include <event2/event.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace cloud {

class Server;

class Connection {
public:
    Connection(Server* server, bufferevent* bev, const std::string& peerIp);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    static void readCb(bufferevent* bev, void* ctx);
    static void eventCb(bufferevent* bev, short what, void* ctx);

    void close();
    const std::string& peerIp() const { return peerIp_; }

private:
    void onRead();
    void onEvent(short what);
    void handleFrame(const Frame& frame);
    void handleJson(const std::string& payload);
    void handleChunk(const ChunkPayload& chunk);

    void sendJson(const Json::Value& v);
    void sendError(uint64_t seq, ErrorCode code, const std::string& msg);
    void sendChunk(const std::string& fileId, uint64_t offset, const void* data, uint32_t len);

    void requireLogin(uint64_t seq);
    std::string resolveUserPath(const std::string& logical, bool mustExist);

    void onRegister(const Json::Value& root, uint64_t seq);
    void onLogin(const Json::Value& root, uint64_t seq);
    void onLogout(const Json::Value& root, uint64_t seq);
    void onList(const Json::Value& root, uint64_t seq);
    void onMkdir(const Json::Value& root, uint64_t seq);
    void onRm(const Json::Value& root, uint64_t seq);
    void onStat(const Json::Value& root, uint64_t seq);
    void onRename(const Json::Value& root, uint64_t seq);
    void onInstantQuery(const Json::Value& root, uint64_t seq);
    void onInstantUpload(const Json::Value& root, uint64_t seq);
    void onUploadBegin(const Json::Value& root, uint64_t seq);
    void onUploadEnd(const Json::Value& root, uint64_t seq);
    void onDownloadBegin(const Json::Value& root, uint64_t seq);
    void onDownloadChunk(const Json::Value& root, uint64_t seq);

    Server* server_;
    bufferevent* bev_;
    std::string peerIp_;
    UserSession session_;
    bool authed_ = false;
    UploadSession upload_;
    DownloadSession download_;
};

}  // namespace cloud
