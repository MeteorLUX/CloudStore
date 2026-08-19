#include "auth.h"
#include "crypto_util.h"
#include "json_util.h"
#include "logger.h"
#include "utils.h"

#include <cstdlib>
#include <sstream>

namespace cloud {

AuthService::AuthService(MysqlPool& mysql, RedisStore& redis, const Config& cfg)
    : mysql_(mysql), redis_(redis), cfg_(cfg) {}

static bool validUsername(const std::string& u) {
    if (u.size() < 3 || u.size() > 32) {
        return false;
    }
    for (char c : u) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

void AuthService::ensureUserRoot(const std::string& username, std::string& rootDir) {
    rootDir = joinPath(cfg_.usersRoot(), username);
    if (!ensureDir(rootDir)) {
        throw CloudError(ErrorCode::Internal, "cannot create user root");
    }
}

void AuthService::logLogin(int userId, const std::string& ip, bool ok) {
    try {
        auto conn = mysql_.acquire();
        std::ostringstream sql;
        sql << "INSERT INTO login_log(user_id, ip, success) VALUES(" << userId << ",'"
            << mysqlEscape(conn.get(), ip) << "'," << (ok ? 1 : 0) << ")";
        mysql_query(conn.get(), sql.str().c_str());
    } catch (...) {
        logWarn("write login_log failed");
    }
}

UserSession AuthService::registerUser(const std::string& username, const std::string& password) {
    if (!validUsername(username) || password.size() < 6) {
        throw CloudError(ErrorCode::BadRequest, "invalid username or password");
    }
    std::string salt = randomHex(16);
    std::string hash = hashPassword(salt, password);
    std::string rootDir;
    ensureUserRoot(username, rootDir);

    auto conn = mysql_.acquire();
    std::ostringstream sql;
    sql << "INSERT INTO users(username, password_hash, salt, root_dir) VALUES('"
        << mysqlEscape(conn.get(), username) << "','" << hash << "','" << salt << "','"
        << mysqlEscape(conn.get(), rootDir) << "')";
    if (mysql_query(conn.get(), sql.str().c_str()) != 0) {
        std::string err = mysql_error(conn.get());
        if (err.find("Duplicate") != std::string::npos) {
            throw CloudError(ErrorCode::Conflict, "username exists");
        }
        throw CloudError(ErrorCode::Internal, "register failed: " + err);
    }
    UserSession s;
    s.userId = static_cast<int>(mysql_insert_id(conn.get()));
    s.username = username;
    s.rootDir = rootDir;
    logInfo("user registered: " + username);
    return s;
}

UserSession AuthService::login(const std::string& username, const std::string& password,
                               const std::string& ip) {
    auto conn = mysql_.acquire();
    std::ostringstream sql;
    sql << "SELECT id, password_hash, salt, root_dir FROM users WHERE username='"
        << mysqlEscape(conn.get(), username) << "' LIMIT 1";
    if (mysql_query(conn.get(), sql.str().c_str()) != 0) {
        throw CloudError(ErrorCode::Internal, mysql_error(conn.get()));
    }
    MYSQL_RES* res = mysql_store_result(conn.get());
    if (!res) {
        throw CloudError(ErrorCode::Internal, "login query has no result");
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        mysql_free_result(res);
        logLogin(0, ip, false);
        throw CloudError(ErrorCode::Unauthorized, "invalid username or password");
    }
    int uid = std::atoi(row[0]);
    std::string hash = row[1] ? row[1] : "";
    std::string salt = row[2] ? row[2] : "";
    std::string root = row[3] ? row[3] : "";
    mysql_free_result(res);

    if (!verifyPassword(salt, password, hash)) {
        logLogin(uid, ip, false);
        throw CloudError(ErrorCode::Unauthorized, "invalid username or password");
    }
    ensureUserRoot(username, root);

    UserSession s;
    s.userId = uid;
    s.username = username;
    s.rootDir = root;
    s.token = randomHex(32);

    Json::Value js;
    js["user_id"] = uid;
    js["username"] = username;
    js["root_dir"] = root;
    js["token"] = s.token;
    redis_.setSession(s.token, dumpJson(js), cfg_.sessionTtlSec);
    logLogin(uid, ip, true);
    logInfo("login ok user=" + username + " ip=" + ip);
    return s;
}

UserSession AuthService::checkToken(const std::string& token) {
    if (token.empty()) {
        throw CloudError(ErrorCode::Unauthorized, "missing token");
    }
    auto raw = redis_.getSession(token);
    if (raw.empty()) {
        throw CloudError(ErrorCode::Unauthorized, "session expired");
    }
    Json::Value js;
    if (!parseJson(raw, js)) {
        throw CloudError(ErrorCode::Unauthorized, "bad session");
    }
    UserSession s;
    s.userId = js.get("user_id", 0).asInt();
    s.username = js.get("username", "").asString();
    s.rootDir = js.get("root_dir", "").asString();
    s.token = token;
    redis_.setSession(token, raw, cfg_.sessionTtlSec);  // sliding ttl
    return s;
}

void AuthService::logout(const std::string& token) { redis_.delSession(token); }

void AuthService::ensureDefaultUsers() {
    auto conn = mysql_.acquire();
    if (mysql_query(conn.get(), "SELECT COUNT(*) FROM users") != 0) {
        throw std::runtime_error(mysql_error(conn.get()));
    }
    MYSQL_RES* res = mysql_store_result(conn.get());
    MYSQL_ROW row = res ? mysql_fetch_row(res) : nullptr;
    long count = (row && row[0]) ? std::atol(row[0]) : 0;
    if (res) {
        mysql_free_result(res);
    }
    if (count > 0) {
        return;
    }
    registerUser("admin", "admin123");
    registerUser("alice", "alice123");
    registerUser("bob", "bob123");
    logInfo("seeded default users: admin / alice / bob");
}

}  // namespace cloud
