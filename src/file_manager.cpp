#include "file_manager.h"
#include "common.h"
#include "utils.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace cloud {

std::string FileManager::fileTypeFromMode(uint32_t mode) {
    if (S_ISREG(mode)) {
        return "file";
    }
    if (S_ISDIR(mode)) {
        return "dir";
    }
    if (S_ISLNK(mode)) {
        return "symlink";
    }
    if (S_ISFIFO(mode)) {
        return "fifo";
    }
    if (S_ISSOCK(mode)) {
        return "socket";
    }
    if (S_ISCHR(mode)) {
        return "chr";
    }
    if (S_ISBLK(mode)) {
        return "blk";
    }
    return "unknown";
}

FileEntry FileManager::statPath(const std::string& physical) {
    struct stat st {};
    if (lstat(physical.c_str(), &st) != 0) {
        throw CloudError(ErrorCode::NotFound, "stat failed: " + physical);
    }
    FileEntry e;
    e.name = baseName(physical);
    e.path = physical;
    e.type = fileTypeFromMode(st.st_mode);
    e.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
    e.mtime = static_cast<int64_t>(st.st_mtime);
    e.mode = static_cast<uint32_t>(st.st_mode);
    return e;
}

void FileManager::listDirRecursive(const std::string& physical, const std::string& logicalPrefix,
                                   std::vector<FileEntry>& out, bool recursive) {
    DIR* dir = opendir(physical.c_str());
    if (!dir) {
        throw CloudError(ErrorCode::NotFound, "opendir failed: " + physical);
    }
    while (dirent* ent = readdir(dir)) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        std::string child = joinPath(physical, name);
        struct stat st {};
        if (lstat(child.c_str(), &st) != 0) {
            continue;
        }
        FileEntry e;
        e.name = name;
        e.path = logicalPrefix.empty() ? ("/" + name) : (logicalPrefix + "/" + name);
        e.type = fileTypeFromMode(st.st_mode);
        e.size = S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
        e.mtime = static_cast<int64_t>(st.st_mtime);
        e.mode = static_cast<uint32_t>(st.st_mode);
        out.push_back(e);
        if (recursive && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            listDirRecursive(child, e.path, out, true);
        }
    }
    closedir(dir);
}

std::vector<FileEntry> FileManager::listDir(const std::string& physical, bool recursive) {
    std::vector<FileEntry> out;
    listDirRecursive(physical, "", out, recursive);
    return out;
}

void FileManager::makeDir(const std::string& physical) {
    if (mkdir(physical.c_str(), 0755) != 0) {
        if (errno == EEXIST) {
            throw CloudError(ErrorCode::Conflict, "directory exists");
        }
        throw CloudError(ErrorCode::Internal, std::string("mkdir failed: ") + std::strerror(errno));
    }
}

static void removeRecursive(const std::string& physical) {
    struct stat st {};
    if (lstat(physical.c_str(), &st) != 0) {
        throw CloudError(ErrorCode::NotFound, "remove target missing");
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR* dir = opendir(physical.c_str());
        if (!dir) {
            throw CloudError(ErrorCode::Internal, "opendir failed during rm");
        }
        while (dirent* ent = readdir(dir)) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") {
                continue;
            }
            removeRecursive(joinPath(physical, name));
        }
        closedir(dir);
        if (rmdir(physical.c_str()) != 0) {
            throw CloudError(ErrorCode::Internal, std::string("rmdir failed: ") + std::strerror(errno));
        }
    } else {
        if (unlink(physical.c_str()) != 0) {
            throw CloudError(ErrorCode::Internal, std::string("unlink failed: ") + std::strerror(errno));
        }
    }
}

void FileManager::removePath(const std::string& physical) { removeRecursive(physical); }

void FileManager::renamePath(const std::string& from, const std::string& to) {
    if (fileExists(to)) {
        throw CloudError(ErrorCode::Conflict, "destination exists");
    }
    if (rename(from.c_str(), to.c_str()) != 0) {
        throw CloudError(ErrorCode::Internal, std::string("rename failed: ") + std::strerror(errno));
    }
}

}  // namespace cloud
