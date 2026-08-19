#pragma once

#include <string>

namespace cloud {

std::string md5Hex(const void* data, size_t len);
std::string md5File(const std::string& path);
std::string md5FileRange(const std::string& path, uint64_t offset, uint32_t len);
std::string sha256Hex(const std::string& data);
std::string hashPassword(const std::string& saltHex, const std::string& password);
bool verifyPassword(const std::string& saltHex, const std::string& password,
                    const std::string& expectedHash);

}  // namespace cloud
