#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cloud {

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
std::string joinPath(const std::string& a, const std::string& b);
bool startsWithPath(const std::string& path, const std::string& root);
std::string toHex(const uint8_t* data, size_t len);
std::vector<uint8_t> fromHex(const std::string& hex);
std::string randomHex(size_t nbytes);
uint64_t nowSec();
std::string nowString();
bool ensureDir(const std::string& path);
std::string parentDir(const std::string& path);
std::string baseName(const std::string& path);
bool fileExists(const std::string& path);
uint64_t fileSize(const std::string& path);
void writeU64Be(uint8_t* p, uint64_t v);
uint64_t readU64Be(const uint8_t* p);
void writeU32Be(uint8_t* p, uint32_t v);
uint32_t readU32Be(const uint8_t* p);
void writeU16Be(uint8_t* p, uint16_t v);
uint16_t readU16Be(const uint8_t* p);

}  // namespace cloud
