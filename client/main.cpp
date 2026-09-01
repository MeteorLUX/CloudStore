#include "client.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void usage(const char* argv0) {
    std::cerr
        << "CloudStore client\n"
        << "Usage:\n"
        << "  " << argv0 << " [--host IP] [--port N]            # interactive shell\n"
        << "  " << argv0 << " --host IP --port N --user U --pass P put <local> <remote>\n"
        << "  " << argv0 << " --host IP --port N --user U --pass P get <remote> <local>\n"
        << "  " << argv0 << " --host IP --port N --user U --pass P ls [path]\n"
        << "Commands in shell: login, register, ls, mkdir, rm, stat, mv, put, get, logout, help, quit\n";
}

static std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> t;
    std::string x;
    while (iss >> x) {
        t.push_back(x);
    }
    return t;
}

static void runShell(cloud::CloudClient& c) {
    std::cout << "CloudStore client. Type 'help'.\n";
    std::string line;
    while (true) {
        std::cout << (c.loggedIn() ? c.username() : "guest") << "> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }
        auto t = tokenize(line);
        if (t.empty()) {
            continue;
        }
        try {
            const auto& cmd = t[0];
            if (cmd == "quit" || cmd == "exit") {
                break;
            }
            if (cmd == "help") {
                usage("cloud_client");
                continue;
            }
            if (cmd == "login" && t.size() >= 3) {
                c.login(t[1], t[2]);
                std::cout << "login ok, user=" << c.username() << "\n";
            } else if (cmd == "register" && t.size() >= 3) {
                c.signup(t[1], t[2]);
                std::cout << "register ok, please login\n";
            } else if (cmd == "logout") {
                c.logout();
            } else if (cmd == "ls") {
                c.ls(t.size() >= 2 ? t[1] : "/", t.size() >= 3 && t[2] == "-r");
            } else if (cmd == "mkdir" && t.size() >= 2) {
                c.mkdir(t[1]);
            } else if (cmd == "rm" && t.size() >= 2) {
                c.rm(t[1]);
            } else if (cmd == "stat" && t.size() >= 2) {
                c.stat(t[1]);
            } else if ((cmd == "mv" || cmd == "rename") && t.size() >= 3) {
                c.rename(t[1], t[2]);
            } else if (cmd == "put" && t.size() >= 3) {
                bool ow = t.size() >= 4 && t[3] == "--overwrite";
                c.put(t[1], t[2], ow);
            } else if (cmd == "get" && t.size() >= 3) {
                c.get(t[1], t[2]);
            } else {
                std::cout << "unknown or incomplete command, type help\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
        }
    }
}

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t port = 9000;
    std::string user;
    std::string pass;
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        if ((std::strcmp(argv[i], "--host") == 0 || std::strcmp(argv[i], "-H") == 0) && i + 1 < argc) {
            host = argv[++i];
        } else if ((std::strcmp(argv[i], "--port") == 0 || std::strcmp(argv[i], "-p") == 0) &&
                   i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((std::strcmp(argv[i], "--user") == 0 || std::strcmp(argv[i], "-u") == 0) &&
                   i + 1 < argc) {
            user = argv[++i];
        } else if ((std::strcmp(argv[i], "--pass") == 0 || std::strcmp(argv[i], "-P") == 0) &&
                   i + 1 < argc) {
            pass = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            rest.emplace_back(argv[i]);
        }
    }

    try {
        cloud::CloudClient client;
        client.connectTo(host, port);
        if (!user.empty()) {
            client.login(user, pass);
        }
        if (rest.empty()) {
            runShell(client);
            return 0;
        }
        const auto& cmd = rest[0];
        if (cmd == "put" && rest.size() >= 3) {
            client.put(rest[1], rest[2], rest.size() >= 4 && rest[3] == "--overwrite");
        } else if (cmd == "get" && rest.size() >= 3) {
            client.get(rest[1], rest[2]);
        } else if (cmd == "ls") {
            client.ls(rest.size() >= 2 ? rest[1] : "/");
        } else if (cmd == "mkdir" && rest.size() >= 2) {
            client.mkdir(rest[1]);
        } else if (cmd == "rm" && rest.size() >= 2) {
            client.rm(rest[1]);
        } else if (cmd == "stat" && rest.size() >= 2) {
            client.stat(rest[1]);
        } else {
            usage(argv[0]);
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
