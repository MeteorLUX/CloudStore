#include "server.h"
#include "connection.h"
#include "logger.h"
#include "utils.h"

#include <event2/bufferevent.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>

#include <chrono>
#include <cstring>
#include <thread>

namespace cloud {

Server::Server(Config cfg) : cfg_(std::move(cfg)) {
    ensureDir(cfg_.storageRoot);
    ensureDir(cfg_.usersRoot());
    ensureDir(cfg_.objectsRoot());
    ensureDir(cfg_.tmpRoot());

    std::string lastErr;
    for (int i = 0; i < 30; ++i) {
        try {
            mysql_ = std::make_unique<MysqlPool>(cfg_);
            redis_ = std::make_unique<RedisStore>(cfg_);
            if (!redis_->ping()) {
                throw std::runtime_error("redis ping failed");
            }
            lastErr.clear();
            break;
        } catch (const std::exception& e) {
            lastErr = e.what();
            logWarn("waiting for mysql/redis: " + lastErr);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (!mysql_ || !redis_) {
        throw std::runtime_error("backend not ready: " + lastErr);
    }

    auth_ = std::make_unique<AuthService>(*mysql_, *redis_, cfg_);
    auth_->ensureDefaultUsers();
    transfer_ = std::make_unique<TransferService>(*redis_, *mysql_, cfg_);
}

Server::~Server() {
    stop();
    for (auto* c : connections_) {
        delete c;
    }
    connections_.clear();
    if (listener_) {
        evconnlistener_free(listener_);
    }
    if (sigint_) {
        event_free(sigint_);
    }
    if (sigterm_) {
        event_free(sigterm_);
    }
    if (base_) {
        event_base_free(base_);
    }
}

void Server::acceptCb(evconnlistener*, evutil_socket_t fd, sockaddr* addr, int socklen, void* ctx) {
    static_cast<Server*>(ctx)->onAccept(fd, addr, socklen);
}

void Server::signalCb(evutil_socket_t, short, void* ctx) { static_cast<Server*>(ctx)->stop(); }

void Server::onAccept(evutil_socket_t fd, sockaddr* addr, int) {
    if (static_cast<int>(connections_.size()) >= cfg_.maxConnections) {
        logWarn("max connections reached");
        evutil_closesocket(fd);
        return;
    }
    char ip[64] = {0};
    if (addr->sa_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(addr)->sin_addr, ip, sizeof(ip));
    } else {
        std::strncpy(ip, "unknown", sizeof(ip) - 1);
    }
    bufferevent* bev = bufferevent_socket_new(base_, fd, BEV_OPT_CLOSE_ON_FREE);
    if (!bev) {
        evutil_closesocket(fd);
        return;
    }
    auto* conn = new Connection(this, bev, ip);
    connections_.insert(conn);
    logInfo(std::string("accept ") + ip + " total=" + std::to_string(connections_.size()));
}

void Server::removeConnection(Connection* conn) {
    connections_.erase(conn);
    delete conn;
}

void Server::stop() {
    if (base_) {
        event_base_loopbreak(base_);
    }
}

int Server::run() {
    evthread_use_pthreads();
    base_ = event_base_new();
    if (!base_) {
        logError("event_base_new failed");
        return 1;
    }

    sockaddr_in sin {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(cfg_.listenPort);
    if (evutil_inet_pton(AF_INET, cfg_.listenIp.c_str(), &sin.sin_addr) != 1) {
        logError("bad listen_ip");
        return 1;
    }

    listener_ = evconnlistener_new_bind(
        base_, &Server::acceptCb, this, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1,
        reinterpret_cast<sockaddr*>(&sin), static_cast<int>(sizeof(sin)));
    if (!listener_) {
        logError("listen failed on " + cfg_.listenIp + ":" + std::to_string(cfg_.listenPort));
        return 1;
    }

    sigint_ = evsignal_new(base_, SIGINT, &Server::signalCb, this);
    sigterm_ = evsignal_new(base_, SIGTERM, &Server::signalCb, this);
    event_add(sigint_, nullptr);
    event_add(sigterm_, nullptr);

    logInfo("CloudStore listening on " + cfg_.listenIp + ":" + std::to_string(cfg_.listenPort));
    event_base_dispatch(base_);
    logInfo("server stopped");
    return 0;
}

}  // namespace cloud
