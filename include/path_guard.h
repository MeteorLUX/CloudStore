#pragma once

#include <string>

namespace cloud {

// 路径边界校验：把用户逻辑路径映射到物理路径，并保证永不越出用户根目录。
class PathGuard {
public:
    // userRoot 必须是已存在的绝对目录。logical 为用户视角路径，允许以 / 开头。
    // mustExist=false 时允许最后一级尚不存在（上传/创建）。
    static std::string resolve(const std::string& userRoot, const std::string& logical,
                               bool mustExist = false);

    // 规范化逻辑路径，若尝试逃逸根目录则返回空 optional 语义：抛异常。
    static std::string normalizeLogical(const std::string& logical);

    static bool isInside(const std::string& path, const std::string& root);
};

}  // namespace cloud
