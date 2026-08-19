#pragma once

#include "config.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

struct redisContext;

namespace cloud {

class RedisStore {
public:
    explicit RedisStore(const Config& cfg);
    ~RedisStore();

    RedisStore(const RedisStore&) = delete;
    RedisStore& operator=(const RedisStore&) = delete;

    bool ping();

    void setSession(const std::string& token, const std::string& json, int ttlSec);
    std::string getSession(const std::string& token);
    void delSession(const std::string& token);

    // 内容寻址对象：md5 -> object 物理路径 / 大小
    void setObject(const std::string& md5, const std::string& objectPath, uint64_t size);
    std::string getObjectPath(const std::string& md5);
    uint64_t getObjectSize(const std::string& md5);
    void deleteObjectMeta(const std::string& md5);

    // 引用集合：成员为 "userId|virtualPath"，SCARD 即引用计数
    void addRef(const std::string& md5, int userId, const std::string& virtualPath);
    long long removeRef(const std::string& md5, int userId, const std::string& virtualPath);
    long long refCount(const std::string& md5);

    void setPathMd5(int userId, const std::string& virtualPath, const std::string& md5);
    std::string getPathMd5(int userId, const std::string& virtualPath);
    void delPathMd5(int userId, const std::string& virtualPath);

    // 秒传持有证明：每个登录会话对每个 md5 发一次随机切片挑战
    void setChallenge(const std::string& token, const std::string& md5, uint64_t offset,
                      uint32_t len);
    bool getChallenge(const std::string& token, const std::string& md5, uint64_t& offset,
                      uint32_t& len);
    void delChallenge(const std::string& token, const std::string& md5);

private:
    std::string cmdGet(const std::string& key);
    void cmdSetEx(const std::string& key, const std::string& value, int ttl);
    void cmdSet(const std::string& key, const std::string& value);
    void cmdDel(const std::string& key);
    long long cmdSAdd(const std::string& key, const std::string& member);
    long long cmdSRem(const std::string& key, const std::string& member);
    long long cmdSCard(const std::string& key);
    void ensure();

    Config cfg_;
    std::mutex mu_;
    redisContext* ctx_ = nullptr;
};

}  // namespace cloud
