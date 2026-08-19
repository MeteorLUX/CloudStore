#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace cloud {

enum class ErrorCode : int {
    Ok = 0,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    Conflict = 409,
    PayloadTooLarge = 413,
    Unprocessable = 422,
    Internal = 500,
    Unavailable = 503
};

class CloudError : public std::runtime_error {
public:
    CloudError(ErrorCode code, const std::string& msg)
        : std::runtime_error(msg), code_(code) {}

    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};

struct UserSession {
    int userId = 0;
    std::string username;
    std::string rootDir;  // 用户逻辑根目录的物理绝对路径
    std::string token;
};

inline std::string virtualRoot() { return "/"; }

constexpr uint32_t kMaxFrameSize = 2 * 1024 * 1024;
constexpr uint32_t kDefaultChunkSize = 64 * 1024;
constexpr const char* kFrameMagic = "CSTR";

}  // namespace cloud
