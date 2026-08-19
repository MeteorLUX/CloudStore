#pragma once

#include <cstdint>
#include <string>

namespace cloud {

struct Config {
    std::string listenIp = "0.0.0.0";
    uint16_t listenPort = 9000;

    std::string storageRoot = "/var/cloudstore/data";
    std::string logFile = "/var/log/cloudstore/server.log";

    std::string mysqlHost = "127.0.0.1";
    uint16_t mysqlPort = 3306;
    std::string mysqlUser = "cloud";
    std::string mysqlPassword = "cloud123";
    std::string mysqlDb = "cloudstore";
    int mysqlPoolSize = 8;

    std::string redisHost = "127.0.0.1";
    uint16_t redisPort = 6379;
    std::string redisPassword;
    int redisDb = 0;

    uint32_t chunkSize = 65536;
    int maxConnections = 1024;
    int sessionTtlSec = 3600;
    int connIdleTimeoutSec = 300;
    uint64_t maxUploadBytes = 8ULL * 1024 * 1024 * 1024;
    uint32_t challengeBytes = 4096;
    int copyWorkers = 2;
    int copySleepUs = 0;  // 后台拷贝节流，0 表示尽快拷贝

    std::string usersRoot() const { return storageRoot + "/users"; }
    std::string objectsRoot() const { return storageRoot + "/objects"; }
    std::string tmpRoot() const { return storageRoot + "/tmp"; }

    static Config load(const std::string& path);
};

}  // namespace cloud
