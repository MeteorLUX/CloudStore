#include "mysql_pool.h"
#include "logger.h"

#include <stdexcept>

namespace cloud {

MysqlPool::Conn::Conn(MysqlPool* pool, MYSQL* mysql) : pool_(pool), mysql_(mysql) {}

MysqlPool::Conn::Conn(Conn&& other) noexcept : pool_(other.pool_), mysql_(other.mysql_) {
    other.mysql_ = nullptr;
    other.pool_ = nullptr;
}

MysqlPool::Conn::~Conn() {
    if (pool_ && mysql_) {
        pool_->release(mysql_);
    }
}

MysqlPool::MysqlPool(const Config& cfg) : cfg_(cfg) {
    mysql_library_init(0, nullptr, nullptr);
    for (int i = 0; i < cfg_.mysqlPoolSize; ++i) {
        MYSQL* m = connectOne();
        idle_.push(m);
        ++created_;
    }
    logInfo("MySQL pool ready, size=" + std::to_string(created_));
}

MysqlPool::~MysqlPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    while (!idle_.empty()) {
        mysql_close(idle_.front());
        idle_.pop();
    }
    mysql_library_end();
}

MYSQL* MysqlPool::connectOne() {
    MYSQL* m = mysql_init(nullptr);
    if (!m) {
        throw std::runtime_error("mysql_init failed");
    }
    unsigned int timeout = 5;
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    bool reconnect = true;
    mysql_options(m, MYSQL_OPT_RECONNECT, &reconnect);
    if (!mysql_real_connect(m, cfg_.mysqlHost.c_str(), cfg_.mysqlUser.c_str(),
                            cfg_.mysqlPassword.c_str(), cfg_.mysqlDb.c_str(), cfg_.mysqlPort,
                            nullptr, 0)) {
        std::string err = mysql_error(m);
        mysql_close(m);
        throw std::runtime_error("mysql connect failed: " + err);
    }
    return m;
}

MysqlPool::Conn MysqlPool::acquire() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [&] { return stopping_ || !idle_.empty(); });
    if (stopping_) {
        throw std::runtime_error("mysql pool stopping");
    }
    MYSQL* m = idle_.front();
    idle_.pop();
    lock.unlock();
    if (mysql_ping(m) != 0) {
        mysql_close(m);
        m = connectOne();
    }
    return Conn(this, m);
}

void MysqlPool::release(MYSQL* mysql) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        idle_.push(mysql);
    }
    cv_.notify_one();
}

bool MysqlPool::ping() {
    auto c = acquire();
    return mysql_ping(c.get()) == 0;
}

std::string mysqlEscape(MYSQL* mysql, const std::string& s) {
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long n = mysql_real_escape_string(mysql, &out[0], s.c_str(), s.size());
    out.resize(n);
    return out;
}

}  // namespace cloud
