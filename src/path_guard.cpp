#include "path_guard.h"
#include "common.h"
#include "utils.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cloud {

static bool hasNul(const std::string& s) { return s.find('\0') != std::string::npos; }

std::string PathGuard::normalizeLogical(const std::string& logical) {
    if (hasNul(logical)) {
        throw CloudError(ErrorCode::Forbidden, "invalid path");
    }
    auto parts = split(logical, '/');
    std::vector<std::string> st;
    for (const auto& c : parts) {
        if (c.empty() || c == ".") {
            continue;
        }
        if (c == "..") {
            if (st.empty()) {
                throw CloudError(ErrorCode::Forbidden, "path traversal denied");
            }
            st.pop_back();
            continue;
        }
        if (c == "~" || c.find('\\') != std::string::npos) {
            throw CloudError(ErrorCode::Forbidden, "illegal path component");
        }
        st.push_back(c);
    }
    if (st.empty()) {
        return "";
    }
    std::string out;
    for (size_t i = 0; i < st.size(); ++i) {
        if (i) {
            out.push_back('/');
        }
        out += st[i];
    }
    return out;
}

bool PathGuard::isInside(const std::string& path, const std::string& root) {
    return startsWithPath(path, root);
}

static std::string mustRealpath(const std::string& path) {
    char buf[PATH_MAX];
    if (!realpath(path.c_str(), buf)) {
        throw CloudError(ErrorCode::NotFound, "path not found");
    }
    return std::string(buf);
}

std::string PathGuard::resolve(const std::string& userRoot, const std::string& logical,
                               bool mustExist) {
    if (userRoot.empty() || userRoot[0] != '/') {
        throw CloudError(ErrorCode::Internal, "user root is not absolute");
    }
    const std::string root = mustRealpath(userRoot);
    const std::string norm = normalizeLogical(logical);

    std::string cur = root;
    if (norm.empty()) {
        return root;
    }

    auto parts = split(norm, '/');
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& name = parts[i];
        if (name.empty()) {
            continue;
        }
        const bool last = (i + 1 == parts.size());
        std::string next = joinPath(cur, name);

        struct stat st {};
        if (lstat(next.c_str(), &st) != 0) {
            if (last && !mustExist) {
                if (!isInside(cur, root)) {
                    throw CloudError(ErrorCode::Forbidden, "path escaped user root");
                }
                return next;
            }
            throw CloudError(ErrorCode::NotFound, "path not found: /" + norm);
        }

        if (S_ISLNK(st.st_mode)) {
            std::string real = mustRealpath(next);
            if (!isInside(real, root)) {
                throw CloudError(ErrorCode::Forbidden, "symlink escapes user root");
            }
            cur = real;
        } else if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) {
            if (!isInside(next, root) && next != root) {
                throw CloudError(ErrorCode::Forbidden, "path escaped user root");
            }
            cur = next;
        } else {
            cur = next;
        }
    }

    if (!isInside(cur, root)) {
        throw CloudError(ErrorCode::Forbidden, "path escaped user root");
    }
    return cur;
}

}  // namespace cloud
