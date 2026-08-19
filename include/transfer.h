#pragma once

#include "common.h"
#include "config.h"
#include "mysql_pool.h"
#include "redis_store.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
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
    std::string status;  // copying / ready
};

struct CopyJob {
    int userId = 0;
    std::string username;
    std::string md5;
    std::string objectPath;
    std::string destPath;
    std::string virtualPath;
    uint64_t size = 0;
};

// 秒传：客户端只读本地文件做 MD5 / 切片证明，不传文件体。
// 命中后立刻可下载（从全局对象库只读），再由后台线程把独立副本拷进当前用户目录。
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

    void indexFile(int userId, const std::string& virtualPath, const std::string& md5,
                   uint64_t size, bool isDir, const std::string& status);
    void unindex(int userId, const std::string& virtualPath);

private:
    void publishObject(const std::string& md5, const std::string& srcFile, uint64_t size);
    void enqueueCopy(CopyJob job);
    void workerLoop();
    void runCopy(const CopyJob& job);
    void copyFileSlow(const std::string& src, const std::string& dest, uint64_t expectedSize);
    bool isCancelled(int userId, const std::string& virtualPath);
    void cancelCopy(int userId, const std::string& virtualPath);
    std::string jobKey(int userId, const std::string& virtualPath) const;

    RedisStore& redis_;
    MysqlPool& mysql_;
    Config cfg_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<CopyJob> jobs_;
    std::unordered_set<std::string> cancelled_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopping_{false};
};

}  // namespace cloud
