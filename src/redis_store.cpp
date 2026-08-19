#include "redis_store.h"
#include "logger.h"

#include <hiredis/hiredis.h>

#include <stdexcept>

namespace cloud {

RedisStore::RedisStore(const Config& cfg) : cfg_(cfg) { ensure(); }

RedisStore::~RedisStore() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

void RedisStore::ensure() {
    if (ctx_ && ctx_->err == 0) {
        return;
    }
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
    ctx_ = redisConnect(cfg_.redisHost.c_str(), cfg_.redisPort);
    if (!ctx_ || ctx_->err) {
        std::string err = ctx_ ? ctx_->errstr : "alloc failed";
        if (ctx_) {
            redisFree(ctx_);
            ctx_ = nullptr;
        }
        throw std::runtime_error("redis connect failed: " + err);
    }
    if (!cfg_.redisPassword.empty()) {
        redisReply* r =
            static_cast<redisReply*>(redisCommand(ctx_, "AUTH %s", cfg_.redisPassword.c_str()));
        if (!r || r->type == REDIS_REPLY_ERROR) {
            std::string e = r ? r->str : "auth failed";
            if (r) {
                freeReplyObject(r);
            }
            throw std::runtime_error("redis auth failed: " + e);
        }
        freeReplyObject(r);
    }
    redisReply* sel =
        static_cast<redisReply*>(redisCommand(ctx_, "SELECT %d", cfg_.redisDb));
    if (!sel || sel->type == REDIS_REPLY_ERROR) {
        std::string e = sel ? sel->str : "select failed";
        if (sel) {
            freeReplyObject(sel);
        }
        throw std::runtime_error("redis select failed: " + e);
    }
    freeReplyObject(sel);
    logInfo("Redis connected " + cfg_.redisHost + ":" + std::to_string(cfg_.redisPort));
}

bool RedisStore::ping() {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "PING"));
    bool ok = r && r->type == REDIS_REPLY_STATUS && r->str && std::string(r->str) == "PONG";
    if (r) {
        freeReplyObject(r);
    }
    return ok;
}

std::string RedisStore::cmdGet(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "GET %s", key.c_str()));
    std::string out;
    if (r && r->type == REDIS_REPLY_STRING && r->str) {
        out.assign(r->str, r->len);
    }
    if (r) {
        freeReplyObject(r);
    }
    return out;
}

void RedisStore::cmdSetEx(const std::string& key, const std::string& value, int ttl) {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "SETEX %s %d %b", key.c_str(), ttl, value.data(), value.size()));
    if (r) {
        freeReplyObject(r);
    }
}

void RedisStore::cmdSet(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r = static_cast<redisReply*>(
        redisCommand(ctx_, "SET %s %b", key.c_str(), value.data(), value.size()));
    if (r) {
        freeReplyObject(r);
    }
}

void RedisStore::cmdDel(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "DEL %s", key.c_str()));
    if (r) {
        freeReplyObject(r);
    }
}

long long RedisStore::cmdSAdd(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r =
        static_cast<redisReply*>(redisCommand(ctx_, "SADD %s %s", key.c_str(), member.c_str()));
    long long n = (r && r->type == REDIS_REPLY_INTEGER) ? r->integer : 0;
    if (r) {
        freeReplyObject(r);
    }
    return n;
}

long long RedisStore::cmdSRem(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r =
        static_cast<redisReply*>(redisCommand(ctx_, "SREM %s %s", key.c_str(), member.c_str()));
    long long n = (r && r->type == REDIS_REPLY_INTEGER) ? r->integer : 0;
    if (r) {
        freeReplyObject(r);
    }
    return n;
}

long long RedisStore::cmdSCard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    ensure();
    redisReply* r = static_cast<redisReply*>(redisCommand(ctx_, "SCARD %s", key.c_str()));
    long long n = (r && r->type == REDIS_REPLY_INTEGER) ? r->integer : 0;
    if (r) {
        freeReplyObject(r);
    }
    return n;
}

void RedisStore::setSession(const std::string& token, const std::string& json, int ttlSec) {
    cmdSetEx("cs:session:" + token, json, ttlSec);
}

std::string RedisStore::getSession(const std::string& token) {
    return cmdGet("cs:session:" + token);
}

void RedisStore::delSession(const std::string& token) { cmdDel("cs:session:" + token); }

void RedisStore::setObject(const std::string& md5, const std::string& objectPath, uint64_t size) {
    cmdSet("cs:obj:" + md5, objectPath);
    cmdSet("cs:objsize:" + md5, std::to_string(size));
}

std::string RedisStore::getObjectPath(const std::string& md5) { return cmdGet("cs:obj:" + md5); }

uint64_t RedisStore::getObjectSize(const std::string& md5) {
    auto s = cmdGet("cs:objsize:" + md5);
    if (s.empty()) {
        return 0;
    }
    return static_cast<uint64_t>(std::stoull(s));
}

void RedisStore::deleteObjectMeta(const std::string& md5) {
    cmdDel("cs:obj:" + md5);
    cmdDel("cs:objsize:" + md5);
    cmdDel("cs:refs:" + md5);
}

void RedisStore::addRef(const std::string& md5, int userId, const std::string& virtualPath) {
    cmdSAdd("cs:refs:" + md5, std::to_string(userId) + "|" + virtualPath);
}

long long RedisStore::removeRef(const std::string& md5, int userId, const std::string& virtualPath) {
    cmdSRem("cs:refs:" + md5, std::to_string(userId) + "|" + virtualPath);
    return cmdSCard("cs:refs:" + md5);
}

long long RedisStore::refCount(const std::string& md5) { return cmdSCard("cs:refs:" + md5); }

void RedisStore::setPathMd5(int userId, const std::string& virtualPath, const std::string& md5) {
    cmdSet("cs:path:" + std::to_string(userId) + ":" + virtualPath, md5);
}

std::string RedisStore::getPathMd5(int userId, const std::string& virtualPath) {
    return cmdGet("cs:path:" + std::to_string(userId) + ":" + virtualPath);
}

void RedisStore::delPathMd5(int userId, const std::string& virtualPath) {
    cmdDel("cs:path:" + std::to_string(userId) + ":" + virtualPath);
}

void RedisStore::setChallenge(const std::string& token, const std::string& md5, uint64_t offset,
                              uint32_t len) {
    cmdSetEx("cs:chal:" + token + ":" + md5, std::to_string(offset) + ":" + std::to_string(len), 120);
}

bool RedisStore::getChallenge(const std::string& token, const std::string& md5, uint64_t& offset,
                              uint32_t& len) {
    auto s = cmdGet("cs:chal:" + token + ":" + md5);
    auto pos = s.find(':');
    if (pos == std::string::npos) {
        return false;
    }
    offset = static_cast<uint64_t>(std::stoull(s.substr(0, pos)));
    len = static_cast<uint32_t>(std::stoul(s.substr(pos + 1)));
    return true;
}

void RedisStore::delChallenge(const std::string& token, const std::string& md5) {
    cmdDel("cs:chal:" + token + ":" + md5);
}

}  // namespace cloud
