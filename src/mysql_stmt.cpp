#include "mysql_stmt.h"

#include <cstring>
#include <stdexcept>

namespace cloud {

static void throwStmtError(MYSQL_STMT* stmt, const char* what) {
    throw std::runtime_error(std::string(what) + ": " + mysql_stmt_error(stmt));
}

MysqlStmt::MysqlStmt(MYSQL* mysql, const std::string& sql) : mysql_(mysql) {
    stmt_ = mysql_stmt_init(mysql_);
    if (!stmt_) {
        throw std::runtime_error("mysql_stmt_init failed");
    }
    if (mysql_stmt_prepare(stmt_, sql.c_str(), sql.size()) != 0) {
        throwStmtError(stmt_, "mysql_stmt_prepare");
    }
    const unsigned long count = mysql_stmt_param_count(stmt_);
    paramBinds_.resize(count);
    std::memset(paramBinds_.data(), 0, sizeof(MYSQL_BIND) * count);
}

MysqlStmt::~MysqlStmt() {
    if (stmt_) {
        mysql_stmt_close(stmt_);
    }
}

void MysqlStmt::bindParam(unsigned index, enum enum_field_types type, void* buffer,
                          unsigned long length, bool isUnsigned) {
    if (index == 0 || index > paramBinds_.size()) {
        throw std::runtime_error("bind param index out of range");
    }
    MYSQL_BIND& b = paramBinds_[index - 1];
    std::memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = type;
    b.buffer = buffer;
    b.is_unsigned = isUnsigned;
    if (type == MYSQL_TYPE_STRING || type == MYSQL_TYPE_VAR_STRING) {
        b.length = &stringParamLengths_[index - 1];
        *b.length = length;
    }
}

void MysqlStmt::bindInt(unsigned index, int value) {
    if (intParamStorage_.size() < index) {
        intParamStorage_.resize(index);
    }
    intParamStorage_[index - 1] = value;
    bindParam(index, MYSQL_TYPE_LONG, &intParamStorage_[index - 1], 0);
}

void MysqlStmt::bindInt64(unsigned index, int64_t value) {
    if (int64ParamStorage_.size() < index) {
        int64ParamStorage_.resize(index);
    }
    int64ParamStorage_[index - 1] = value;
    bindParam(index, MYSQL_TYPE_LONGLONG, &int64ParamStorage_[index - 1], 0);
}

void MysqlStmt::bindUInt64(unsigned index, uint64_t value) {
    if (uint64ParamStorage_.size() < index) {
        uint64ParamStorage_.resize(index);
    }
    uint64ParamStorage_[index - 1] = value;
    bindParam(index, MYSQL_TYPE_LONGLONG, &uint64ParamStorage_[index - 1], 0, true);
}

void MysqlStmt::bindString(unsigned index, const std::string& value) {
    if (stringParamStorage_.size() < index) {
        stringParamStorage_.resize(index);
        stringParamLengths_.resize(index, 0);
    }
    stringParamStorage_[index - 1] = value;
    stringParamLengths_[index - 1] = value.size();
    bindParam(index, MYSQL_TYPE_STRING, stringParamStorage_[index - 1].data(),
              static_cast<unsigned long>(value.size()));
}

void MysqlStmt::execute() {
    if (!paramBinds_.empty()) {
        if (mysql_stmt_bind_param(stmt_, paramBinds_.data()) != 0) {
            throwStmtError(stmt_, "mysql_stmt_bind_param");
        }
    }
    if (mysql_stmt_execute(stmt_) != 0) {
        throwStmtError(stmt_, "mysql_stmt_execute");
    }
    if (mysql_stmt_store_result(stmt_) != 0) {
        throwStmtError(stmt_, "mysql_stmt_store_result");
    }
    if (!resultBinds_.empty() && mysql_stmt_bind_result(stmt_, resultBinds_.data()) != 0) {
        throwStmtError(stmt_, "mysql_stmt_bind_result");
    }
}

uint64_t MysqlStmt::insertId() const {
    return mysql_stmt_insert_id(stmt_);
}

void MysqlStmt::bindResultString(unsigned index, std::string& out, unsigned long& length) {
    if (stringResultStorage_.size() < index) {
        stringResultStorage_.resize(index);
        stringResultLengths_.resize(index, 0);
        resultBinds_.resize(index);
        resultStringOut_.resize(index, nullptr);
        resultStringLenOut_.resize(index, nullptr);
    }
    stringResultStorage_[index - 1].assign(4096, '\0');
    stringResultLengths_[index - 1] = 0;
    MYSQL_BIND& b = resultBinds_[index - 1];
    std::memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_STRING;
    b.buffer = stringResultStorage_[index - 1].data();
    b.buffer_length = stringResultStorage_[index - 1].size();
    b.length = &stringResultLengths_[index - 1];
    resultStringOut_[index - 1] = &out;
    resultStringLenOut_[index - 1] = &length;
}

void MysqlStmt::bindResultInt(unsigned index, int& out) {
    if (intResultStorage_.size() < index) {
        intResultStorage_.resize(index);
        resultBinds_.resize(index);
        resultIntOut_.resize(index, nullptr);
    }
    MYSQL_BIND& b = resultBinds_[index - 1];
    std::memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_LONG;
    b.buffer = &intResultStorage_[index - 1];
    resultIntOut_[index - 1] = &out;
}

void MysqlStmt::bindResultInt64(unsigned index, int64_t& out) {
    if (int64ResultStorage_.size() < index) {
        int64ResultStorage_.resize(index);
        resultBinds_.resize(index);
        resultInt64Out_.resize(index, nullptr);
    }
    MYSQL_BIND& b = resultBinds_[index - 1];
    std::memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.buffer = &int64ResultStorage_[index - 1];
    resultInt64Out_[index - 1] = &out;
}

void MysqlStmt::bindResultUInt64(unsigned index, uint64_t& out) {
    if (uint64ResultStorage_.size() < index) {
        uint64ResultStorage_.resize(index);
        resultBinds_.resize(index);
        resultUInt64Out_.resize(index, nullptr);
    }
    MYSQL_BIND& b = resultBinds_[index - 1];
    std::memset(&b, 0, sizeof(MYSQL_BIND));
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.buffer = &uint64ResultStorage_[index - 1];
    b.is_unsigned = true;
    resultUInt64Out_[index - 1] = &out;
}

int MysqlStmt::fetch() {
    const int rc = mysql_stmt_fetch(stmt_);
    if (rc == 0) {
        for (size_t i = 0; i < resultStringOut_.size(); ++i) {
            if (resultStringOut_[i]) {
                *resultStringOut_[i] =
                    std::string(stringResultStorage_[i].data(), stringResultLengths_[i]);
                if (resultStringLenOut_[i]) {
                    *resultStringLenOut_[i] = stringResultLengths_[i];
                }
            }
        }
        for (size_t i = 0; i < resultIntOut_.size(); ++i) {
            if (resultIntOut_[i]) {
                *resultIntOut_[i] = intResultStorage_[i];
            }
        }
        for (size_t i = 0; i < resultInt64Out_.size(); ++i) {
            if (resultInt64Out_[i]) {
                *resultInt64Out_[i] = int64ResultStorage_[i];
            }
        }
        for (size_t i = 0; i < resultUInt64Out_.size(); ++i) {
            if (resultUInt64Out_[i]) {
                *resultUInt64Out_[i] = uint64ResultStorage_[i];
            }
        }
    }
    return rc;
}

}  // namespace cloud
