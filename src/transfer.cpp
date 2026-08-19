#include "transfer.h"
#include "crypto_util.h"
#include "file_manager.h"
#include "logger.h"
#include "path_guard.h"
#include "utils.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

namespace cloud {

TransferService::TransferService(RedisStore& redis, MysqlPool& mysql, const Config& cfg)
    : redis_(redis), mysql_(mysql), cfg_(cfg) {
    ensureDir(cfg_.objectsRoot());
    ensureDir(cfg_.tmpRoot());
    ensureDir(cfg_.usersRoot());
    int n = cfg_.copyWorkers > 0 ? cfg_.copyWorkers : 1;
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
    logInfo("transfer service ready, copy workers=" + std::to_string(n));
}

TransferService::~TransferService() {
    stopping_ = true;
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
}

std::string TransferService::objectPathFor(const std::string& md5) const {
    if (md5.size() < 2) {
        throw CloudError(ErrorCode::BadRequest, "bad md5");
    }
    return joinPath(joinPath(cfg_.objectsRoot(), md5.substr(0, 2)), md5);
}

std::string TransferService::jobKey(int userId, const std::string& virtualPath) const {
    return std::to_string(userId) + "|" + virtualPath;
}

bool TransferService::isCancelled(int userId, const std::string& virtualPath) {
    std::lock_guard<std::mutex> lock(mu_);
    return cancelled_.count(jobKey(userId, virtualPath)) > 0;
}

void TransferService::cancelCopy(int userId, const std::string& virtualPath) {
    std::lock_guard<std::mutex> lock(mu_);
    cancelled_.insert(jobKey(userId, virtualPath));
}

void TransferService::enqueueCopy(CopyJob job) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        cancelled_.erase(jobKey(job.userId, job.virtualPath));
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

void TransferService::workerLoop() {
    while (!stopping_) {
        CopyJob job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        try {
            runCopy(job);
        } catch (const std::exception& e) {
            logError("background copy failed " + job.virtualPath + ": " + e.what());
        }
    }
}

void TransferService::copyFileSlow(const std::string& src, const std::string& dest,
                                   uint64_t expectedSize) {
    int in = ::open(src.c_str(), O_RDONLY);
    if (in < 0) {
        throw CloudError(ErrorCode::NotFound, "object missing during copy");
    }
    std::string tmp = dest + ".copying";
    ensureDir(parentDir(tmp));
    int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        ::close(in);
        throw CloudError(ErrorCode::Internal, std::string("open dest failed: ") + std::strerror(errno));
    }
    std::vector<char> buf(cfg_.chunkSize ? cfg_.chunkSize : 65536);
    uint64_t copied = 0;
    while (true) {
        ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            ::close(in);
            ::close(out);
            ::unlink(tmp.c_str());
            throw CloudError(ErrorCode::Internal, "read object failed");
        }
        if (n == 0) {
            break;
        }
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::write(out, buf.data() + off, static_cast<size_t>(n - off));
            if (w < 0) {
                ::close(in);
                ::close(out);
                ::unlink(tmp.c_str());
                throw CloudError(ErrorCode::Internal, "write user copy failed");
            }
            off += w;
        }
        copied += static_cast<uint64_t>(n);
        if (cfg_.copySleepUs > 0) {
            usleep(static_cast<useconds_t>(cfg_.copySleepUs));
        }
    }
    ::fsync(out);
    ::close(in);
    ::close(out);
    if (expectedSize && copied != expectedSize) {
        ::unlink(tmp.c_str());
        throw CloudError(ErrorCode::Unprocessable, "copy size mismatch");
    }
    if (::rename(tmp.c_str(), dest.c_str()) != 0) {
        ::unlink(tmp.c_str());
        throw CloudError(ErrorCode::Internal, "rename user copy failed");
    }
}

void TransferService::runCopy(const CopyJob& job) {
    if (isCancelled(job.userId, job.virtualPath)) {
        logInfo("copy cancelled " + job.virtualPath);
        return;
    }
    copyFileSlow(job.objectPath, job.destPath, job.size);
    if (isCancelled(job.userId, job.virtualPath)) {
        ::unlink(job.destPath.c_str());
        return;
    }
    indexFile(job.userId, job.virtualPath, job.md5, job.size, false, "ready");
    redis_.setPathMd5(job.userId, job.virtualPath, job.md5);
    logInfo("background copy ready user=" + job.username + " path=" + job.virtualPath);
}

void TransferService::publishObject(const std::string& md5, const std::string& srcFile,
                                    uint64_t size) {
    std::string obj = objectPathFor(md5);
    if (fileExists(obj) && fileSize(obj) == size) {
        redis_.setObject(md5, obj, size);
        return;
    }
    ensureDir(parentDir(obj));
    copyFileSlow(srcFile, obj, size);
    if (::chmod(obj.c_str(), 0444) != 0) {
        logWarn("chmod object failed: " + obj);
    }
    redis_.setObject(md5, obj, size);
}

InstantQuery TransferService::queryInstant(const UserSession& session, const std::string& md5,
                                           uint64_t size) {
    InstantQuery q;
    if (md5.size() != 32) {
        throw CloudError(ErrorCode::BadRequest, "md5 must be 32 hex chars");
    }
    std::string obj = redis_.getObjectPath(md5);
    if (obj.empty()) {
        obj = objectPathFor(md5);
    }
    if (!fileExists(obj) || fileSize(obj) != size) {
        q.hit = false;
        return q;
    }
    q.hit = true;
    q.size = size;
    uint32_t clen = cfg_.challengeBytes;
    if (clen == 0) {
        clen = 4096;
    }
    if (size == 0) {
        q.challengeOffset = 0;
        q.challengeLen = 0;
    } else {
        if (clen > size) {
            clen = static_cast<uint32_t>(size);
        }
        uint64_t span = size > clen ? (size - clen) : 0;
        uint64_t seed = 0;
        for (char c : md5 + session.token) {
            seed = seed * 131 + static_cast<unsigned char>(c);
        }
        q.challengeOffset = span ? (seed % span) : 0;
        q.challengeLen = clen;
    }
    redis_.setChallenge(session.token, md5, q.challengeOffset, q.challengeLen);
    return q;
}

void TransferService::instantUpload(const UserSession& session, const std::string& destPath,
                                    const std::string& virtualPath, const std::string& md5,
                                    uint64_t size, uint64_t proofOffset, const std::string& proofMd5,
                                    bool overwrite) {
    uint64_t expectOff = 0;
    uint32_t expectLen = 0;
    if (!redis_.getChallenge(session.token, md5, expectOff, expectLen)) {
        throw CloudError(ErrorCode::BadRequest, "missing instant challenge, call instant_query first");
    }
    if (proofOffset != expectOff) {
        throw CloudError(ErrorCode::Forbidden, "challenge offset mismatch");
    }
    std::string obj = redis_.getObjectPath(md5);
    if (obj.empty() || !fileExists(obj)) {
        throw CloudError(ErrorCode::NotFound, "object not in store");
    }
    if (fileSize(obj) != size) {
        throw CloudError(ErrorCode::Unprocessable, "size mismatch");
    }
    if (expectLen > 0) {
        std::string real = md5FileRange(obj, expectOff, expectLen);
        if (real != proofMd5) {
            throw CloudError(ErrorCode::Forbidden, "possession proof failed");
        }
    }
    redis_.delChallenge(session.token, md5);

    if (fileExists(destPath)) {
        if (!overwrite) {
            throw CloudError(ErrorCode::Conflict, "destination exists");
        }
        cancelCopy(session.userId, virtualPath);
        ::unlink(destPath.c_str());
        unindex(session.userId, virtualPath);
    }

    ensureDir(parentDir(destPath));
    indexFile(session.userId, virtualPath, md5, size, false, "copying");
    redis_.setPathMd5(session.userId, virtualPath, md5);

    CopyJob job;
    job.userId = session.userId;
    job.username = session.username;
    job.md5 = md5;
    job.objectPath = obj;
    job.destPath = destPath;
    job.virtualPath = virtualPath;
    job.size = size;
    enqueueCopy(std::move(job));
    logInfo("instant upload accepted, background copy queued path=" + virtualPath);
}

UploadSession TransferService::beginUpload(const UserSession& session, const std::string& destPath,
                                           const std::string& virtualPath, const std::string& md5,
                                           uint64_t size) {
    if (size > cfg_.maxUploadBytes) {
        throw CloudError(ErrorCode::PayloadTooLarge, "file too large");
    }
    if (md5.size() != 32) {
        throw CloudError(ErrorCode::BadRequest, "md5 must be 32 hex chars");
    }
    std::string tmpDir = joinPath(cfg_.tmpRoot(), session.username);
    ensureDir(tmpDir);
    UploadSession st;
    st.fileId = randomHex(16);
    st.destPath = destPath;
    st.virtualPath = virtualPath;
    st.md5 = md5;
    st.totalSize = size;
    st.partPath = joinPath(tmpDir, md5 + "_" + baseName(destPath) + ".part");
    ensureDir(parentDir(destPath));

    int flags = O_RDWR | O_CREAT;
    st.fd = ::open(st.partPath.c_str(), flags, 0644);
    if (st.fd < 0) {
        throw CloudError(ErrorCode::Internal, std::string("open part failed: ") + std::strerror(errno));
    }
    struct stat stbuf {};
    if (fstat(st.fd, &stbuf) == 0) {
        st.received = static_cast<uint64_t>(stbuf.st_size);
        if (st.received > size) {
            st.received = 0;
            if (ftruncate(st.fd, 0) != 0) {
                logWarn("ftruncate part failed");
            }
        }
    }
    return st;
}

void TransferService::writeChunk(UploadSession& st, uint64_t offset, const void* data, uint32_t len) {
    if (st.fd < 0) {
        throw CloudError(ErrorCode::BadRequest, "no active upload");
    }
    if (offset + len > st.totalSize) {
        throw CloudError(ErrorCode::BadRequest, "chunk out of range");
    }
    ssize_t n = pwrite(st.fd, data, len, static_cast<off_t>(offset));
    if (n != static_cast<ssize_t>(len)) {
        throw CloudError(ErrorCode::Internal, "pwrite failed");
    }
    uint64_t end = offset + len;
    if (end > st.received) {
        st.received = end;
    }
}

void TransferService::finishUpload(const UserSession& session, UploadSession& st) {
    if (st.fd < 0) {
        throw CloudError(ErrorCode::BadRequest, "no active upload");
    }
    ::fsync(st.fd);
    ::close(st.fd);
    st.fd = -1;

    if (fileSize(st.partPath) != st.totalSize) {
        throw CloudError(ErrorCode::Unprocessable, "uploaded size mismatch");
    }
    std::string realMd5 = md5File(st.partPath);
    if (realMd5 != st.md5) {
        ::unlink(st.partPath.c_str());
        throw CloudError(ErrorCode::Unprocessable, "md5 mismatch, transfer corrupted");
    }

    publishObject(st.md5, st.partPath, st.totalSize);
    copyFileSlow(st.partPath, st.destPath, st.totalSize);
    ::unlink(st.partPath.c_str());
    indexFile(session.userId, st.virtualPath, st.md5, st.totalSize, false, "ready");
    redis_.setPathMd5(session.userId, st.virtualPath, st.md5);
    logInfo("upload complete path=" + st.virtualPath + " md5=" + st.md5);
}

void TransferService::closeUpload(UploadSession& st) {
    if (st.fd >= 0) {
        ::close(st.fd);
        st.fd = -1;
    }
}

DownloadSession TransferService::beginDownload(const UserSession& session,
                                               const std::string& physical,
                                               const std::string& virtualPath) {
    DownloadSession d;
    d.fileId = randomHex(16);
    std::string status = fileStatus(session.userId, virtualPath);
    std::string src = physical;
    if (status == "copying" || !fileExists(physical)) {
        std::string md5 = redis_.getPathMd5(session.userId, virtualPath);
        if (md5.empty()) {
            throw CloudError(ErrorCode::NotFound, "file not found");
        }
        src = objectPathFor(md5);
        auto cached = redis_.getObjectPath(md5);
        if (!cached.empty()) {
            src = cached;
        }
        if (!fileExists(src)) {
            throw CloudError(ErrorCode::NotFound, "object missing, file not readable yet");
        }
    }
    d.srcPath = src;
    d.totalSize = fileSize(src);
    d.fd = ::open(src.c_str(), O_RDONLY);
    if (d.fd < 0) {
        throw CloudError(ErrorCode::Internal, "open download failed");
    }
    return d;
}

uint32_t TransferService::readChunk(DownloadSession& st, uint64_t offset, void* buf, uint32_t len) {
    if (st.fd < 0) {
        throw CloudError(ErrorCode::BadRequest, "no active download");
    }
    if (offset >= st.totalSize) {
        return 0;
    }
    if (offset + len > st.totalSize) {
        len = static_cast<uint32_t>(st.totalSize - offset);
    }
    ssize_t n = pread(st.fd, buf, len, static_cast<off_t>(offset));
    if (n < 0) {
        throw CloudError(ErrorCode::Internal, "pread failed");
    }
    return static_cast<uint32_t>(n);
}

void TransferService::closeDownload(DownloadSession& st) {
    if (st.fd >= 0) {
        ::close(st.fd);
        st.fd = -1;
    }
}

void TransferService::indexFile(int userId, const std::string& virtualPath, const std::string& md5,
                                uint64_t size, bool isDir, const std::string& status) {
    auto conn = mysql_.acquire();
    std::ostringstream sql;
    sql << "INSERT INTO file_index(user_id, virtual_path, md5, size, is_dir, status) VALUES("
        << userId << ",'" << mysqlEscape(conn.get(), virtualPath) << "','"
        << mysqlEscape(conn.get(), md5) << "'," << size << "," << (isDir ? 1 : 0) << ",'"
        << mysqlEscape(conn.get(), status)
        << "') ON DUPLICATE KEY UPDATE md5=VALUES(md5), size=VALUES(size), is_dir=VALUES(is_dir), "
           "status=VALUES(status), updated_at=NOW()";
    if (mysql_query(conn.get(), sql.str().c_str()) != 0) {
        throw CloudError(ErrorCode::Internal, std::string("index failed: ") + mysql_error(conn.get()));
    }
}

void TransferService::unindex(int userId, const std::string& virtualPath) {
    auto conn = mysql_.acquire();
    std::ostringstream sql;
    sql << "DELETE FROM file_index WHERE user_id=" << userId << " AND virtual_path='"
        << mysqlEscape(conn.get(), virtualPath) << "'";
    mysql_query(conn.get(), sql.str().c_str());
    redis_.delPathMd5(userId, virtualPath);
}

std::string TransferService::fileStatus(int userId, const std::string& virtualPath) {
    auto conn = mysql_.acquire();
    std::ostringstream sql;
    sql << "SELECT status FROM file_index WHERE user_id=" << userId << " AND virtual_path='"
        << mysqlEscape(conn.get(), virtualPath) << "' LIMIT 1";
    if (mysql_query(conn.get(), sql.str().c_str()) != 0) {
        return "";
    }
    MYSQL_RES* res = mysql_store_result(conn.get());
    if (!res) {
        return "";
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    std::string st = (row && row[0]) ? row[0] : "";
    mysql_free_result(res);
    return st;
}

std::vector<PendingFile> TransferService::listIndexed(int userId, const std::string& dirLogical) {
    std::string prefix = dirLogical;
    if (prefix.empty() || prefix == "/") {
        prefix = "/";
    }
    if (prefix.back() != '/') {
        prefix.push_back('/');
    }
    auto conn = mysql_.acquire();
    std::ostringstream sql;
    sql << "SELECT virtual_path, md5, size, status FROM file_index WHERE user_id=" << userId
        << " AND is_dir=0 AND (virtual_path='" << mysqlEscape(conn.get(), dirLogical == "/" ? "/" : dirLogical)
        << "' OR virtual_path LIKE '" << mysqlEscape(conn.get(), prefix) << "%')";
    std::vector<PendingFile> out;
    if (mysql_query(conn.get(), sql.str().c_str()) != 0) {
        return out;
    }
    MYSQL_RES* res = mysql_store_result(conn.get());
    if (!res) {
        return out;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        PendingFile f;
        f.virtualPath = row[0] ? row[0] : "";
        f.md5 = row[1] ? row[1] : "";
        f.size = row[2] ? std::strtoull(row[2], nullptr, 10) : 0;
        f.status = row[3] ? row[3] : "";
        // 仅返回当前目录这一层（秒传尚未落盘的 copying 项需要显示）
        auto rest = f.virtualPath;
        if (prefix != "/" && rest.compare(0, prefix.size(), prefix) == 0) {
            rest = rest.substr(prefix.size() - 1);  // keep leading /
            if (!rest.empty() && rest[0] == '/') {
                rest = rest.substr(1);
            }
        } else if (prefix == "/" && !rest.empty() && rest[0] == '/') {
            rest = rest.substr(1);
        }
        if (rest.find('/') != std::string::npos) {
            continue;
        }
        out.push_back(f);
    }
    mysql_free_result(res);
    return out;
}

void TransferService::removeUserPath(const UserSession& session, const std::string& physical,
                                     const std::string& virtualPath) {
    struct stat st {};
    bool exists = lstat(physical.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        auto kids = FileManager::listDir(physical, true);
        for (const auto& e : kids) {
            if (e.type == "file") {
                std::string v = virtualPath == "/" ? e.path : (virtualPath + e.path);
                // e.path already like /name/...
                cancelCopy(session.userId, e.path);
                unindex(session.userId, e.path);
            }
        }
        FileManager::removePath(physical);
        unindex(session.userId, virtualPath);
        return;
    }
    cancelCopy(session.userId, virtualPath);
    if (exists) {
        if (::unlink(physical.c_str()) != 0 && errno != ENOENT) {
            throw CloudError(ErrorCode::Internal, std::string("unlink failed: ") + std::strerror(errno));
        }
        ::unlink((physical + ".copying").c_str());
    }
    unindex(session.userId, virtualPath);
}

void TransferService::renameUserPath(const UserSession& session, const std::string& fromPhysical,
                                     const std::string& fromVirtual, const std::string& toPhysical,
                                     const std::string& toVirtual) {
    if (fileStatus(session.userId, fromVirtual) == "copying") {
        throw CloudError(ErrorCode::Conflict, "file is still being copied into your directory");
    }
    FileManager::renamePath(fromPhysical, toPhysical);
    auto md5 = redis_.getPathMd5(session.userId, fromVirtual);
    auto sz = fileSize(toPhysical);
    unindex(session.userId, fromVirtual);
    indexFile(session.userId, toVirtual, md5, sz, false, "ready");
    if (!md5.empty()) {
        redis_.setPathMd5(session.userId, toVirtual, md5);
    }
}

}  // namespace cloud
