#include "crypto_util.h"
#include "utils.h"

#include <openssl/evp.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace cloud {

static std::string digestHex(const EVP_MD* md, const void* data, size_t len) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outLen = 0;
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, out, &outLen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("digest failed");
    }
    EVP_MD_CTX_free(ctx);
    return toHex(out, outLen);
}

std::string md5Hex(const void* data, size_t len) {
    return digestHex(EVP_md5(), data, len);
}

std::string md5FileRange(const std::string& path, uint64_t offset, uint32_t len) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open file for md5 range: " + path);
    }
    if (!in.seekg(static_cast<std::streamoff>(offset))) {
        throw std::runtime_error("seek failed for md5 range: " + path);
    }
    std::vector<char> buf(len);
    in.read(buf.data(), static_cast<std::streamsize>(len));
    auto n = in.gcount();
    if (n != static_cast<std::streamsize>(len)) {
        throw std::runtime_error("short read for md5 range: " + path);
    }
    return md5Hex(buf.data(), static_cast<size_t>(n));
}

std::string md5File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open file for md5: " + path);
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
        if (ctx) {
            EVP_MD_CTX_free(ctx);
        }
        throw std::runtime_error("md5 init failed");
    }
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto n = in.gcount();
        if (n > 0 && EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n)) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("md5 update failed");
        }
    }
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outLen = 0;
    if (EVP_DigestFinal_ex(ctx, out, &outLen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("md5 final failed");
    }
    EVP_MD_CTX_free(ctx);
    return toHex(out, outLen);
}

std::string sha256Hex(const std::string& data) {
    return digestHex(EVP_sha256(), data.data(), data.size());
}

std::string hashPassword(const std::string& saltHex, const std::string& password) {
    return sha256Hex(saltHex + password);
}

bool verifyPassword(const std::string& saltHex, const std::string& password,
                    const std::string& expectedHash) {
    return hashPassword(saltHex, password) == expectedHash;
}

}  // namespace cloud
