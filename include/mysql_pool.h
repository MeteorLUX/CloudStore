#pragma once

#include "config.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include <mysql/mysql.h>

namespace cloud {

class MysqlPool {
public:
    explicit MysqlPool(const Config& cfg);
    ~MysqlPool();

    MysqlPool(const MysqlPool&) = delete;
    MysqlPool& operator=(const MysqlPool&) = delete;

    class Conn {
    public:
        Conn(MysqlPool* pool, MYSQL* mysql);
        Conn(Conn&& other) noexcept;
        Conn& operator=(Conn&&) = delete;
        ~Conn();

        MYSQL* get() const { return mysql_; }
        MYSQL* operator->() const { return mysql_; }

    private:
        MysqlPool* pool_ = nullptr;
        MYSQL* mysql_ = nullptr;
    };

    Conn acquire();
    bool ping();

private:
    MYSQL* connectOne();
    void release(MYSQL* mysql);

    Config cfg_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<MYSQL*> idle_;
    int created_ = 0;
    bool stopping_ = false;
};

}  // namespace cloud
