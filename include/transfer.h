#pragma once

#include "common.h"
#include "config.h"
#include "mysql_pool.h"
#include "redis_store.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cloud {

struct UploadSession {
    std::string fileId;
    std::string destPath;
    std::string virtualPath;
    std::string partPath;
    std::string md5;
    uint64_t totalSize = 0;
    uint64_t received = 0;
    int fd = -1;
};

struct DownloadSession {
    std::string fileId;
    std::string srcPath;
    uint64_t totalSize = 0;
    int fd = -1;
};

struct InstantQuery {
    bool hit = false;
    uint64_t size = 0;
    uint64_t challengeOffset = 0;
    uint32_t challengeLen = 0;
};

struct PendingFile {
    std::string virtualPath;
    std::string md5;
    uint64_t size = 0;
    std::string status;  // ready
};

struct IndexedFile {
    std::string virtualPath;
    std::string md5;
    uint64_t size = 0;
    bool isDir = false;
    std::string status;
};

// 秒传与上传均采用引用计数：相同内容在 objects/ 只存一份，
// file_index 记录 user_id + virtual_path -> md5，Redis SCARD 维护引用数。
class TransferService {
public:
    TransferService(RedisStore& redis, MysqlPool& mysql, const Config& cfg);
    ~TransferService();

    std::string objectPathFor(const std::string& md5) const;

    InstantQuery queryInstant(const UserSession& session, const std::string& md5, uint64_t size);

    void instantUpload(const UserSession& session, const std::string& destPath,
                       const std::string& virtualPath, const std::string& md5, uint64_t size,
                       uint64_t proofOffset, const std::string& proofMd5, bool overwrite);

    UploadSession beginUpload(const UserSession& session, const std::string& destPath,
                              const std::string& virtualPath, const std::string& md5,
                              uint64_t size);
    void writeChunk(UploadSession& st, uint64_t offset, const void* data, uint32_t len);
    void finishUpload(const UserSession& session, UploadSession& st);
    void closeUpload(UploadSession& st);

    DownloadSession beginDownload(const UserSession& session, const std::string& physical,
                                  const std::string& virtualPath);
    uint32_t readChunk(DownloadSession& st, uint64_t offset, void* buf, uint32_t len);
    void closeDownload(DownloadSession& st);

    void removeUserPath(const UserSession& session, const std::string& physical,
                        const std::string& virtualPath);
    void renameUserPath(const UserSession& session, const std::string& fromPhysical,
                        const std::string& fromVirtual, const std::string& toPhysical,
                        const std::string& toVirtual);

    std::vector<PendingFile> listIndexed(int userId, const std::string& dirLogical);
    std::string fileStatus(int userId, const std::string& virtualPath);
    bool lookupFile(int userId, const std::string& virtualPath, IndexedFile& out);

    void indexFile(int userId, const std::string& virtualPath, const std::string& md5,
                   uint64_t size, bool isDir, const std::string& status);
    void unindex(int userId, const std::string& virtualPath);

private:
    void publishObject(const std::string& md5, const std::string& srcFile, uint64_t size);
    void copyToObjectStore(const std::string& src, const std::string& dest, uint64_t expectedSize);
    void addContentRef(int userId, const std::string& virtualPath, const std::string& md5,
                       uint64_t size);
    void releaseContentRef(int userId, const std::string& virtualPath);
    std::string resolveObjectPath(const std::string& md5) const;
    void releaseIndexedUnderPrefix(int userId, const std::string& dirVirtual);
    void renameIndexedFile(int userId, const std::string& fromVirtual,
                           const std::string& toVirtual, const std::string& md5, uint64_t size);

    RedisStore& redis_;
    MysqlPool& mysql_;
    Config cfg_;
};

}  // namespace cloud
