#include "config.h"
#include "utils.h"

#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace cloud {

static std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

Config Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open config: " + path);
    }

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        kv[trim(line.substr(0, pos))] = unquote(trim(line.substr(pos + 1)));
    }

    auto get = [&](const std::string& k, const std::string& def) {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    };
    auto geti = [&](const std::string& k, long long def) {
        auto it = kv.find(k);
        return it == kv.end() ? def : std::stoll(it->second);
    };

    Config c;
    c.listenIp = get("listen_ip", c.listenIp);
    c.listenPort = static_cast<uint16_t>(geti("listen_port", c.listenPort));
    c.storageRoot = get("storage_root", c.storageRoot);
    c.logFile = get("log_file", c.logFile);
    c.mysqlHost = get("mysql_host", c.mysqlHost);
    c.mysqlPort = static_cast<uint16_t>(geti("mysql_port", c.mysqlPort));
    c.mysqlUser = get("mysql_user", c.mysqlUser);
    c.mysqlPassword = get("mysql_password", c.mysqlPassword);
    c.mysqlDb = get("mysql_db", c.mysqlDb);
    c.mysqlPoolSize = static_cast<int>(geti("mysql_pool_size", c.mysqlPoolSize));
    c.redisHost = get("redis_host", c.redisHost);
    c.redisPort = static_cast<uint16_t>(geti("redis_port", c.redisPort));
    c.redisPassword = get("redis_password", c.redisPassword);
    c.redisDb = static_cast<int>(geti("redis_db", c.redisDb));
    c.chunkSize = static_cast<uint32_t>(geti("chunk_size", c.chunkSize));
    c.maxConnections = static_cast<int>(geti("max_connections", c.maxConnections));
    c.sessionTtlSec = static_cast<int>(geti("session_ttl_sec", c.sessionTtlSec));
    c.connIdleTimeoutSec = static_cast<int>(geti("conn_idle_timeout_sec", c.connIdleTimeoutSec));
    c.maxUploadBytes = static_cast<uint64_t>(geti("max_upload_bytes", static_cast<long long>(c.maxUploadBytes)));
    c.challengeBytes = static_cast<uint32_t>(geti("challenge_bytes", c.challengeBytes));
    c.copyWorkers = static_cast<int>(geti("copy_workers", c.copyWorkers));
    c.copySleepUs = static_cast<int>(geti("copy_sleep_us", c.copySleepUs));
    return c;
}

}  // namespace cloud
