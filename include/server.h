#pragma once

#include "auth.h"
#include "config.h"
#include "file_manager.h"
#include "mysql_pool.h"
#include "redis_store.h"
#include "transfer.h"

#include <event2/event.h>
#include <event2/listener.h>

#include <memory>
#include <string>
#include <unordered_set>

namespace cloud {

class Connection;

class Server {
public:
    explicit Server(Config cfg);
    ~Server();

    int run();
    void stop();

    AuthService& auth() { return *auth_; }
    TransferService& transfer() { return *transfer_; }
    RedisStore& redis() { return *redis_; }
    const Config& config() const { return cfg_; }

    void removeConnection(Connection* conn);

private:
    static void acceptCb(evconnlistener* listener, evutil_socket_t fd, sockaddr* addr,
                         int socklen, void* ctx);
    static void signalCb(evutil_socket_t fd, short what, void* ctx);

    void onAccept(evutil_socket_t fd, sockaddr* addr, int socklen);

    Config cfg_;
    event_base* base_ = nullptr;
    evconnlistener* listener_ = nullptr;
    event* sigint_ = nullptr;
    event* sigterm_ = nullptr;

    std::unique_ptr<MysqlPool> mysql_;
    std::unique_ptr<RedisStore> redis_;
    std::unique_ptr<AuthService> auth_;
    std::unique_ptr<TransferService> transfer_;
    std::unordered_set<Connection*> connections_;
};

}  // namespace cloud
