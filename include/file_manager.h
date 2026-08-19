#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cloud {

struct FileEntry {
    std::string name;
    std::string path;
    std::string type;  // file / dir / symlink / fifo / socket / chr / blk / unknown
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t mode = 0;
};

class FileManager {
public:
    static FileEntry statPath(const std::string& physical);
    static std::vector<FileEntry> listDir(const std::string& physical, bool recursive);
    static void makeDir(const std::string& physical);
    static void removePath(const std::string& physical);
    static void renamePath(const std::string& from, const std::string& to);
    static std::string fileTypeFromMode(uint32_t mode);

private:
    static void listDirRecursive(const std::string& physical, const std::string& logicalPrefix,
                                 std::vector<FileEntry>& out, bool recursive);
};

}  // namespace cloud
