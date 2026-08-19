#pragma once

#include "common.h"
#include "config.h"
#include "mysql_pool.h"
#include "redis_store.h"

#include <string>

namespace cloud {

class AuthService {
public:
    AuthService(MysqlPool& mysql, RedisStore& redis, const Config& cfg);

    UserSession login(const std::string& username, const std::string& password,
                      const std::string& ip);
    UserSession registerUser(const std::string& username, const std::string& password);
    UserSession checkToken(const std::string& token);
    void logout(const std::string& token);
    void ensureDefaultUsers();

private:
    void ensureUserRoot(const std::string& username, std::string& rootDir);
    void logLogin(int userId, const std::string& ip, bool ok);

    MysqlPool& mysql_;
    RedisStore& redis_;
    Config cfg_;
};

}  // namespace cloud
