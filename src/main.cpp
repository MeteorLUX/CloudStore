#include "config.h"
#include "logger.h"
#include "server.h"

#include <cstring>
#include <iostream>
#include <string>

static void usage(const char* argv0) {
    std::cerr << "CloudStore server\n"
              << "Usage: " << argv0 << " [-c conf/server.conf]\n";
}

int main(int argc, char** argv) {
    std::string conf = "conf/server.conf";
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "-c") == 0 || std::strcmp(argv[i], "--conf") == 0) &&
            i + 1 < argc) {
            conf = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    try {
        auto cfg = cloud::Config::load(conf);
        cloud::Logger::instance().setFile(cfg.logFile);
        cloud::logInfo("starting CloudStore, config=" + conf);
        cloud::Server server(cfg);
        return server.run();
    } catch (const std::exception& e) {
        cloud::logError(std::string("fatal: ") + e.what());
        return 1;
    }
}
