#include "client.h"
#include "json_util.h"

#include <httplib.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct GuiState {
    std::mutex mu;
    cloud::CloudClient client;
    std::string progressPhase;
    uint64_t progressCurrent = 0;
    uint64_t progressTotal = 0;
};

static std::string jsonOk(const Json::Value& data = Json::Value()) {
    Json::Value root;
    root["ok"] = true;
    root["data"] = data;
    return dumpJson(root);
}

static std::string jsonErr(const std::string& msg) {
    Json::Value root;
    root["ok"] = false;
    root["error"] = msg;
    return dumpJson(root);
}

static std::string findWebRoot() {
    const std::vector<std::string> candidates = {
        "client/web",
        "../client/web",
        "../../client/web",
        "./web",
    };
    for (const auto& c : candidates) {
        if (fs::exists(c + "/index.html")) {
            return fs::absolute(c).string();
        }
    }
    return "";
}

static void setCors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

}  // namespace

int main(int argc, char** argv) {
    std::string listenHost = "127.0.0.1";
    int listenPort = 9080;
    std::string serverHost = "127.0.0.1";
    uint16_t serverPort = 9000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--listen" && i + 1 < argc) {
            listenPort = std::atoi(argv[++i]);
        } else if (arg == "--host" && i + 1 < argc) {
            serverHost = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            serverPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: cloud_gui [--listen 9080] [--host 127.0.0.1] [--port 9000]\n";
            return 0;
        }
    }

    const std::string webRoot = findWebRoot();
    if (webRoot.empty()) {
        std::cerr << "cannot find client/web directory\n";
        return 1;
    }

    GuiState state;
    state.client.setProgressCallback([&](uint64_t current, uint64_t total, const std::string& phase) {
        std::lock_guard<std::mutex> lock(state.mu);
        state.progressCurrent = current;
        state.progressTotal = total;
        state.progressPhase = phase;
    });

    httplib::Server svr;

    svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        setCors(res);
        res.status = 204;
    });

    svr.Post("/api/connect", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            Json::Value body;
            if (!parseJson(req.body, body)) {
                res.status = 400;
                res.set_content(jsonErr("bad json"), "application/json");
                return;
            }
            std::string host = body.get("host", serverHost).asString();
            uint16_t port = static_cast<uint16_t>(body.get("port", serverPort).asUInt());
            std::lock_guard<std::mutex> lock(state.mu);
            state.client.close();
            state.client.connectTo(host, port);
            Json::Value data;
            data["host"] = host;
            data["port"] = port;
            res.set_content(jsonOk(data), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Get("/api/status", [&](const httplib::Request&, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        std::lock_guard<std::mutex> lock(state.mu);
        Json::Value data;
        data["connected"] = state.client.connected();
        data["logged_in"] = state.client.loggedIn();
        data["username"] = state.client.username();
        data["host"] = state.client.host();
        data["port"] = state.client.port();
        data["progress_phase"] = state.progressPhase;
        data["progress_current"] = static_cast<Json::UInt64>(state.progressCurrent);
        data["progress_total"] = static_cast<Json::UInt64>(state.progressTotal);
        res.set_content(jsonOk(data), "application/json");
    });

    svr.Post("/api/login", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            Json::Value body;
            parseJson(req.body, body);
            std::lock_guard<std::mutex> lock(state.mu);
            if (!state.client.connected()) {
                state.client.connectTo(serverHost, serverPort);
            }
            auto data = state.client.login(body["username"].asString(), body["password"].asString());
            res.set_content(jsonOk(data), "application/json");
        } catch (const std::exception& e) {
            res.status = 401;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Post("/api/register", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            Json::Value body;
            parseJson(req.body, body);
            std::lock_guard<std::mutex> lock(state.mu);
            if (!state.client.connected()) {
                state.client.connectTo(serverHost, serverPort);
            }
            auto data =
                state.client.signup(body["username"].asString(), body["password"].asString());
            res.set_content(jsonOk(data), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Post("/api/logout", [&](const httplib::Request&, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        std::lock_guard<std::mutex> lock(state.mu);
        state.client.logout();
        res.set_content(jsonOk(), "application/json");
    });

    svr.Get("/api/ls", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            std::string path = req.get_param_value("path");
            if (path.empty()) {
                path = "/";
            }
            std::lock_guard<std::mutex> lock(state.mu);
            auto data = state.client.listEntries(path, false);
            res.set_content(jsonOk(data), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Post("/api/mkdir", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            Json::Value body;
            parseJson(req.body, body);
            std::lock_guard<std::mutex> lock(state.mu);
            auto data = state.client.makeDir(body["path"].asString());
            res.set_content(jsonOk(data), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Post("/api/rm", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            Json::Value body;
            parseJson(req.body, body);
            std::lock_guard<std::mutex> lock(state.mu);
            auto data = state.client.removePath(body["path"].asString());
            res.set_content(jsonOk(data), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Post("/api/rename", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            Json::Value body;
            parseJson(req.body, body);
            std::lock_guard<std::mutex> lock(state.mu);
            auto data = state.client.renamePath(body["from"].asString(), body["to"].asString());
            res.set_content(jsonOk(data), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Post("/api/upload", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        res.set_content_type("application/json");
        try {
            if (!req.form.has_file("file") || !req.form.has_field("path")) {
                res.status = 400;
                res.set_content(jsonErr("missing file or path"), "application/json");
                return;
            }
            const auto& file = req.form.get_file("file");
            std::string remotePath = req.form.get_field("path");
            bool overwrite = req.form.has_field("overwrite") &&
                             req.form.get_field("overwrite") == "true";
            const std::string tmpPath =
                (fs::temp_directory_path() / ("cloudstore_up_" + std::to_string(::getpid()))).string();
            {
                std::ofstream out(tmpPath, std::ios::binary);
                out.write(file.content.data(), static_cast<std::streamsize>(file.content.size()));
            }
            Json::Value result;
            {
                std::lock_guard<std::mutex> lock(state.mu);
                state.progressCurrent = 0;
                state.progressTotal = 0;
                state.progressPhase = "starting";
                result = state.client.put(tmpPath, remotePath, overwrite);
            }
            fs::remove(tmpPath);
            res.set_content(jsonOk(result), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.Get("/api/download", [&](const httplib::Request& req, httplib::Response& res) {
        setCors(res);
        try {
            std::string path = req.get_param_value("path");
            if (path.empty()) {
                res.status = 400;
                res.set_content(jsonErr("missing path"), "application/json");
                return;
            }
            const std::string tmpPath =
                (fs::temp_directory_path() / ("cloudstore_dl_" + std::to_string(::getpid()))).string();
            {
                std::lock_guard<std::mutex> lock(state.mu);
                state.client.get(path, tmpPath);
            }
            std::ifstream in(tmpPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            fs::remove(tmpPath);
            std::string name = path;
            auto pos = name.find_last_of('/');
            if (pos != std::string::npos) {
                name = name.substr(pos + 1);
            }
            res.set_content(content, "application/octet-stream");
            res.set_header("Content-Disposition", "attachment; filename=\"" + name + "\"");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(jsonErr(e.what()), "application/json");
        }
    });

    svr.set_mount_point("/", webRoot);

    std::cout << "CloudStore GUI: http://" << listenHost << ":" << listenPort << "\n";
    std::cout << "Cloud server: " << serverHost << ":" << serverPort << "\n";
    std::cout << "Web root: " << webRoot << "\n";

    if (!svr.listen(listenHost, listenPort)) {
        std::cerr << "failed to listen on " << listenHost << ":" << listenPort << "\n";
        return 1;
    }
    return 0;
}
