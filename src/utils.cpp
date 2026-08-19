#include "utils.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>

namespace cloud {

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) {
        ++a;
    }
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
        --b;
    }
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    if (a.back() == '/' && b.front() == '/') {
        return a + b.substr(1);
    }
    if (a.back() != '/' && b.front() != '/') {
        return a + "/" + b;
    }
    return a + b;
}

bool startsWithPath(const std::string& path, const std::string& root) {
    if (path == root) {
        return true;
    }
    const std::string prefix = root.back() == '/' ? root : root + "/";
    return path.compare(0, prefix.size(), prefix) == 0;
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[data[i] & 0xF];
    }
    return out;
}

std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) {
        return out;
    }
    out.resize(hex.size() / 2);
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::string randomHex(size_t nbytes) {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<uint8_t> buf(nbytes);
    for (size_t i = 0; i < nbytes; ++i) {
        buf[i] = static_cast<uint8_t>(dist(gen));
    }
    return toHex(buf.data(), buf.size());
}

uint64_t nowSec() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

std::string nowString() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

bool ensureDir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::string cur;
    if (path[0] == '/') {
        cur = "/";
    }
    for (const auto& part : split(path, '/')) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (cur == "/") {
            cur += part;
        } else if (cur.empty()) {
            cur = part;
        } else {
            cur += "/" + part;
        }
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            return false;
        }
    }
    return true;
}

std::string parentDir(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

std::string baseName(const std::string& path) {
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

bool fileExists(const std::string& path) {
    struct stat st {};
    return lstat(path.c_str(), &st) == 0;
}

uint64_t fileSize(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_size);
}

void writeU64Be(uint8_t* p, uint64_t v) {
    p[0] = static_cast<uint8_t>(v >> 56);
    p[1] = static_cast<uint8_t>(v >> 48);
    p[2] = static_cast<uint8_t>(v >> 40);
    p[3] = static_cast<uint8_t>(v >> 32);
    p[4] = static_cast<uint8_t>(v >> 24);
    p[5] = static_cast<uint8_t>(v >> 16);
    p[6] = static_cast<uint8_t>(v >> 8);
    p[7] = static_cast<uint8_t>(v);
}

uint64_t readU64Be(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) | (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) | (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) | (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) | static_cast<uint64_t>(p[7]);
}

void writeU32Be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint32_t readU32Be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void writeU16Be(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

uint16_t readU16Be(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

}  // namespace cloud
