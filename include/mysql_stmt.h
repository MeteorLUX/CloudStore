#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <mysql/mysql.h>

namespace cloud {

// MySQL 预编译语句封装：SQL 模板用 ? 占位，参数与语句结构分离，防 SQL 注入。
class MysqlStmt {
public:
    MysqlStmt(MYSQL* mysql, const std::string& sql);
    ~MysqlStmt();

    MysqlStmt(const MysqlStmt&) = delete;
    MysqlStmt& operator=(const MysqlStmt&) = delete;

    void bindInt(unsigned index, int value);
    void bindInt64(unsigned index, int64_t value);
    void bindUInt64(unsigned index, uint64_t value);
    void bindString(unsigned index, const std::string& value);

    void execute();
    uint64_t insertId() const;

    // SELECT：在 execute 前绑定结果列，fetch 返回 0 表示有行
    int fetch();
    void bindResultString(unsigned index, std::string& out, unsigned long& length);
    void bindResultInt(unsigned index, int& out);
    void bindResultInt64(unsigned index, int64_t& out);
    void bindResultUInt64(unsigned index, uint64_t& out);

private:
    void bindParam(unsigned index, enum enum_field_types type, void* buffer, unsigned long length,
                   bool isUnsigned = false);

    MYSQL* mysql_ = nullptr;
    MYSQL_STMT* stmt_ = nullptr;
    std::vector<MYSQL_BIND> paramBinds_;
    std::vector<MYSQL_BIND> resultBinds_;
    std::vector<std::string> stringParamStorage_;
    std::vector<unsigned long> stringParamLengths_;
    std::vector<int64_t> int64ParamStorage_;
    std::vector<int> intParamStorage_;
    std::vector<uint64_t> uint64ParamStorage_;
    std::vector<std::string> stringResultStorage_;
    std::vector<unsigned long> stringResultLengths_;
    std::vector<int64_t> int64ResultStorage_;
    std::vector<int> intResultStorage_;
    std::vector<uint64_t> uint64ResultStorage_;
    std::vector<std::string*> resultStringOut_;
    std::vector<unsigned long*> resultStringLenOut_;
    std::vector<int*> resultIntOut_;
    std::vector<int64_t*> resultInt64Out_;
    std::vector<uint64_t*> resultUInt64Out_;
};

}  // namespace cloud
