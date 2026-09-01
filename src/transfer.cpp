#include "transfer.h"
#include "crypto_util.h"
#include "file_manager.h"
#include "logger.h"
#include "mysql_stmt.h"
#include "path_guard.h"
#include "utils.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cloud {

TransferService::TransferService(RedisStore& redis, MysqlPool& mysql, const Config& cfg)
    : redis_(redis), mysql_(mysql), cfg_(cfg) {
    ensureDir(cfg_.objectsRoot());
    ensureDir(cfg_.tmpRoot());
    ensureDir(cfg_.usersRoot());
    logInfo("transfer service ready (reference-counted object store)");
}

TransferService::~TransferService() = default;

std::string TransferService::objectPathFor(const std::string& md5) const {
    if (md5.size() < 2) {
        throw CloudError(ErrorCode::BadRequest, "bad md5");
    }
    return joinPath(joinPath(cfg_.objectsRoot(), md5.substr(0, 2)), md5);
}

std::string TransferService::resolveObjectPath(const std::string& md5) const {
    auto cached = redis_.getObjectPath(md5);
    if (!cached.empty()) {
        return cached;
    }
    return objectPathFor(md5);
}

void TransferService::copyToObjectStore(const std::string& src, const std::string& dest,
                                      uint64_t expectedSize) {
    int in = ::open(src.c_str(), O_RDONLY);
    if (in < 0) {
        throw CloudError(ErrorCode::NotFound, "source missing for object publish");
    }
    ensureDir(parentDir(dest));
    int out = ::open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        ::close(in);
        throw CloudError(ErrorCode::Internal, std::string("open object failed: ") + std::strerror(errno));
    }
    std::vector<char> buf(cfg_.chunkSize ? cfg_.chunkSize : 65536);
    uint64_t copied = 0;
    while (true) {
        ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            ::close(in);
            ::close(out);
            ::unlink(dest.c_str());
            throw CloudError(ErrorCode::Internal, "read source failed");
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
                ::unlink(dest.c_str());
                throw CloudError(ErrorCode::Internal, "write object failed");
            }
            off += w;
        }
        copied += static_cast<uint64_t>(n);
    }
    ::fsync(out);
    ::close(in);
    ::close(out);
    if (expectedSize && copied != expectedSize) {
        ::unlink(dest.c_str());
        throw CloudError(ErrorCode::Unprocessable, "object size mismatch");
    }
}

void TransferService::publishObject(const std::string& md5, const std::string& srcFile,
                                    uint64_t size) {
    std::string obj = objectPathFor(md5);
    if (fileExists(obj) && fileSize(obj) == size) {
        redis_.setObject(md5, obj, size);
        return;
    }
    ensureDir(parentDir(obj));
    copyToObjectStore(srcFile, obj, size);
    if (::chmod(obj.c_str(), 0444) != 0) {
        logWarn("chmod object failed: " + obj);
    }
    redis_.setObject(md5, obj, size);
}

void TransferService::addContentRef(int userId, const std::string& virtualPath,
                                    const std::string& md5, uint64_t size) {
    redis_.addRef(md5, userId, virtualPath);
    indexFile(userId, virtualPath, md5, size, false, "ready");
    redis_.setPathMd5(userId, virtualPath, md5);
}

void TransferService::releaseContentRef(int userId, const std::string& virtualPath) {
    std::string md5 = redis_.getPathMd5(userId, virtualPath);
    if (md5.empty()) {
        IndexedFile info;
        if (lookupFile(userId, virtualPath, info) && !info.isDir) {
            md5 = info.md5;
        }
    }
    if (!md5.empty()) {
        long long refs = redis_.removeRef(md5, userId, virtualPath);
        if (refs <= 0) {
            std::string obj = resolveObjectPath(md5);
            if (fileExists(obj)) {
                if (::chmod(obj.c_str(), 0644) != 0) {
                    logWarn("chmod before unlink object failed: " + obj);
                }
                if (::unlink(obj.c_str()) != 0 && errno != ENOENT) {
                    logWarn("unlink object failed: " + obj);
                }
            }
            redis_.deleteObjectMeta(md5);
            logInfo("object gc md5=" + md5);
        }
    }
    unindex(userId, virtualPath);
}

void TransferService::releaseIndexedUnderPrefix(int userId, const std::string& dirVirtual) {
    std::string prefix = dirVirtual;
    if (prefix.empty() || prefix == "/") {
        prefix = "/";
    }
    if (prefix.back() != '/') {
        prefix.push_back('/');
    }
    auto conn = mysql_.acquire();
    MysqlStmt stmt(conn.get(),
                   "SELECT virtual_path FROM file_index WHERE user_id=? AND is_dir=0 AND "
                   "virtual_path LIKE ?");
    stmt.bindInt(1, userId);
    stmt.bindString(2, prefix + "%");

    std::string path;
    unsigned long pathLen = 0;
    stmt.bindResultString(1, path, pathLen);
    stmt.execute();

    std::vector<std::string> paths;
    while (stmt.fetch() == 0) {
        paths.push_back(path);
    }
    for (const auto& vp : paths) {
        releaseContentRef(userId, vp);
    }
}

void TransferService::renameIndexedFile(int userId, const std::string& fromVirtual,
                                        const std::string& toVirtual, const std::string& md5,
                                        uint64_t size) {
    redis_.removeRef(md5, userId, fromVirtual);
    redis_.addRef(md5, userId, toVirtual);
    redis_.delPathMd5(userId, fromVirtual);
    indexFile(userId, toVirtual, md5, size, false, "ready");
    redis_.setPathMd5(userId, toVirtual, md5);
    auto conn = mysql_.acquire();
    MysqlStmt stmt(conn.get(),
                   "DELETE FROM file_index WHERE user_id=? AND virtual_path=?");
    stmt.bindInt(1, userId);
    stmt.bindString(2, fromVirtual);
    stmt.execute();
}

InstantQuery TransferService::queryInstant(const UserSession& session, const std::string& md5,
                                           uint64_t size) {
    InstantQuery q;
    if (md5.size() != 32) {
        throw CloudError(ErrorCode::BadRequest, "md5 must be 32 hex chars");
    }
    std::string obj = resolveObjectPath(md5);
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
    std::string obj = resolveObjectPath(md5);
    if (!fileExists(obj)) {
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

    IndexedFile existing;
    bool hasExisting = lookupFile(session.userId, virtualPath, existing) && !existing.isDir;
    if (hasExisting) {
        if (!overwrite) {
            throw CloudError(ErrorCode::Conflict, "destination exists");
        }
        releaseContentRef(session.userId, virtualPath);
    } else if (fileExists(destPath)) {
        if (!overwrite) {
            throw CloudError(ErrorCode::Conflict, "destination exists");
        }
        ::unlink(destPath.c_str());
    }

    ensureDir(parentDir(destPath));
    addContentRef(session.userId, virtualPath, md5, size);
    logInfo("instant upload ref added path=" + virtualPath + " md5=" + md5);
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
    ensureDir(parentDir(destPath));
    UploadSession st;
    st.fileId = randomHex(16);
    st.destPath = destPath;
    st.virtualPath = virtualPath;
    st.md5 = md5;
    st.totalSize = size;
    st.partPath = joinPath(tmpDir, md5 + "_" + baseName(destPath) + ".part");

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

    IndexedFile existing;
    if (lookupFile(session.userId, st.virtualPath, existing) && !existing.isDir) {
        releaseContentRef(session.userId, st.virtualPath);
    } else if (fileExists(st.destPath)) {
        ::unlink(st.destPath.c_str());
    }

    publishObject(st.md5, st.partPath, st.totalSize);
    ::unlink(st.partPath.c_str());
    addContentRef(session.userId, st.virtualPath, st.md5, st.totalSize);
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
    (void)physical;
    DownloadSession d;
    d.fileId = randomHex(16);
    std::string md5 = redis_.getPathMd5(session.userId, virtualPath);
    if (md5.empty()) {
        IndexedFile info;
        if (!lookupFile(session.userId, virtualPath, info) || info.isDir || info.md5.empty()) {
            throw CloudError(ErrorCode::NotFound, "file not found");
        }
        md5 = info.md5;
    }
    std::string src = resolveObjectPath(md5);
    if (!fileExists(src)) {
        throw CloudError(ErrorCode::NotFound, "object missing");
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
    MysqlStmt stmt(conn.get(),
                   "INSERT INTO file_index(user_id, virtual_path, md5, size, is_dir, status) "
                   "VALUES(?, ?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE md5=VALUES(md5), "
                   "size=VALUES(size), is_dir=VALUES(is_dir), status=VALUES(status), "
                   "updated_at=NOW()");
    stmt.bindInt(1, userId);
    stmt.bindString(2, virtualPath);
    stmt.bindString(3, md5);
    stmt.bindUInt64(4, size);
    stmt.bindInt(5, isDir ? 1 : 0);
    stmt.bindString(6, status);
    try {
        stmt.execute();
    } catch (const std::exception& e) {
        throw CloudError(ErrorCode::Internal, std::string("index failed: ") + e.what());
    }
}

void TransferService::unindex(int userId, const std::string& virtualPath) {
    auto conn = mysql_.acquire();
    MysqlStmt stmt(conn.get(), "DELETE FROM file_index WHERE user_id=? AND virtual_path=?");
    stmt.bindInt(1, userId);
    stmt.bindString(2, virtualPath);
    stmt.execute();
    redis_.delPathMd5(userId, virtualPath);
}

bool TransferService::lookupFile(int userId, const std::string& virtualPath, IndexedFile& out) {
    auto conn = mysql_.acquire();
    MysqlStmt stmt(conn.get(),
                   "SELECT virtual_path, md5, size, is_dir, status FROM file_index WHERE user_id=? "
                   "AND virtual_path=? LIMIT 1");
    stmt.bindInt(1, userId);
    stmt.bindString(2, virtualPath);

    std::string vp;
    std::string md5;
    uint64_t size = 0;
    int isDir = 0;
    std::string status;
    unsigned long len1 = 0;
    unsigned long len2 = 0;
    unsigned long len3 = 0;
    stmt.bindResultString(1, vp, len1);
    stmt.bindResultString(2, md5, len2);
    stmt.bindResultUInt64(3, size);
    stmt.bindResultInt(4, isDir);
    stmt.bindResultString(5, status, len3);
    try {
        stmt.execute();
    } catch (...) {
        return false;
    }
    if (stmt.fetch() != 0) {
        return false;
    }
    out.virtualPath = vp;
    out.md5 = md5;
    out.size = size;
    out.isDir = isDir != 0;
    out.status = status;
    return true;
}

std::string TransferService::fileStatus(int userId, const std::string& virtualPath) {
    IndexedFile info;
    if (!lookupFile(userId, virtualPath, info)) {
        return "";
    }
    return info.status;
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
    const std::string exact = (dirLogical == "/" ? "/" : dirLogical);
    MysqlStmt stmt(conn.get(),
                   "SELECT virtual_path, md5, size, status FROM file_index WHERE user_id=? AND "
                   "is_dir=0 AND (virtual_path=? OR virtual_path LIKE ?)");
    stmt.bindInt(1, userId);
    stmt.bindString(2, exact);
    stmt.bindString(3, prefix + "%");

    std::string virtualPath;
    std::string md5;
    uint64_t size = 0;
    std::string status;
    unsigned long len1 = 0;
    unsigned long len2 = 0;
    unsigned long len3 = 0;
    stmt.bindResultString(1, virtualPath, len1);
    stmt.bindResultString(2, md5, len2);
    stmt.bindResultUInt64(3, size);
    stmt.bindResultString(4, status, len3);

    std::vector<PendingFile> out;
    try {
        stmt.execute();
    } catch (...) {
        return out;
    }
    while (stmt.fetch() == 0) {
        PendingFile f;
        f.virtualPath = virtualPath;
        f.md5 = md5;
        f.size = size;
        f.status = status.empty() ? "ready" : status;
        auto rest = f.virtualPath;
        if (prefix != "/" && rest.compare(0, prefix.size(), prefix) == 0) {
            rest = rest.substr(prefix.size() - 1);
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
    return out;
}

void TransferService::removeUserPath(const UserSession& session, const std::string& physical,
                                     const std::string& virtualPath) {
    IndexedFile indexed;
    if (lookupFile(session.userId, virtualPath, indexed) && !indexed.isDir) {
        releaseContentRef(session.userId, virtualPath);
        if (fileExists(physical)) {
            ::unlink(physical.c_str());
        }
        return;
    }

    struct stat st {};
    bool exists = lstat(physical.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        releaseIndexedUnderPrefix(session.userId, virtualPath);
        FileManager::removePath(physical);
        unindex(session.userId, virtualPath);
        return;
    }

    if (exists) {
        if (::unlink(physical.c_str()) != 0 && errno != ENOENT) {
            throw CloudError(ErrorCode::Internal, std::string("unlink failed: ") + std::strerror(errno));
        }
    }
    releaseContentRef(session.userId, virtualPath);
}

void TransferService::renameUserPath(const UserSession& session, const std::string& fromPhysical,
                                     const std::string& fromVirtual, const std::string& toPhysical,
                                     const std::string& toVirtual) {
    IndexedFile info;
    if (lookupFile(session.userId, fromVirtual, info) && !info.isDir) {
        renameIndexedFile(session.userId, fromVirtual, toVirtual, info.md5, info.size);
        return;
    }
    FileManager::renamePath(fromPhysical, toPhysical);
    auto md5 = redis_.getPathMd5(session.userId, fromVirtual);
    uint64_t sz = fileExists(toPhysical) ? fileSize(toPhysical) : 0;
    unindex(session.userId, fromVirtual);
    indexFile(session.userId, toVirtual, md5, sz, false, "ready");
    if (!md5.empty()) {
        redis_.setPathMd5(session.userId, toVirtual, md5);
    }
}

}  // namespace cloud
