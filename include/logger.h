#pragma once

#include <mutex>
#include <string>

namespace cloud {

enum class LogLevel { Debug, Info, Warn, Error };

class Logger {
public:
    static Logger& instance();

    void setFile(const std::string& path);
    void setLevel(LogLevel level);
    void log(LogLevel level, const std::string& msg);

private:
    Logger() = default;
    std::mutex mu_;
    std::string file_;
    LogLevel level_ = LogLevel::Info;
};

void logDebug(const std::string& msg);
void logInfo(const std::string& msg);
void logWarn(const std::string& msg);
void logError(const std::string& msg);

}  // namespace cloud
