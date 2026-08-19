#include "logger.h"
#include "utils.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>

namespace cloud {

Logger& Logger::instance() {
    static Logger g;
    return g;
}

void Logger::setFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    file_ = path;
    auto dir = parentDir(path);
    if (!dir.empty()) {
        ensureDir(dir);
    }
}

void Logger::setLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mu_);
    level_ = level;
}

static const char* levelName(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}

void Logger::log(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    std::string line = std::string(ts) + " [" + levelName(level) + "] " + msg + "\n";
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
    if (!file_.empty()) {
        std::ofstream out(file_, std::ios::app);
        if (out) {
            out << line;
        }
    }
}

void logDebug(const std::string& msg) { Logger::instance().log(LogLevel::Debug, msg); }
void logInfo(const std::string& msg) { Logger::instance().log(LogLevel::Info, msg); }
void logWarn(const std::string& msg) { Logger::instance().log(LogLevel::Warn, msg); }
void logError(const std::string& msg) { Logger::instance().log(LogLevel::Error, msg); }

}  // namespace cloud
