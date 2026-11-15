#include "../kit/runtime.h"
#include "../third_party/http/httplib.h"
#include <algorithm>
#include <csignal>
#include <cctype>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
using namespace wrapper;
namespace {
volatile sig_atomic_t stopping = 0;
void signalStop(int) { stopping = 1; }
struct Options {
    std::string binary, assets, profile, host = "127.0.0.1", instance;
    int port = 12341;
    pid_t owner = 0;
};
Options options(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (i + 1 >= argc)
            throw std::invalid_argument("missing option value");
        std::string v = argv[++i];
        if (key == "--binary")
            o.binary = v;
        else if (key == "--assets")
            o.assets = v;
        else if (key == "--data-dir")
            o.profile = v;
        else if (key == "--host" && (v == "127.0.0.1" || v == "localhost"))
            o.host = v;
        else if (key == "--port")
            o.port = std::stoi(v);
        else if (key == "--owner-pid")
            o.owner = std::stoi(v);
        else if (key == "--instance-id")
            o.instance = v;
        else
            throw std::invalid_argument("invalid server option");
    }
    if (o.binary.empty() || o.assets.empty() || o.profile.empty() || o.port <= 0 || o.port > 65535 || o.owner < 0)
        throw std::invalid_argument("invalid server configuration");
    if (o.instance.empty())
        o.instance = std::to_string(getpid()) + "-" + std::to_string(arc4random());
    return o;
}
void record(const Options &o, const std::string &route, const std::string &method, int code, double ms) noexcept try {
    static const std::set<std::string> methods = {"GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS", "PATCH"};
    Json j = {{"endpoint", route},
              {"method", methods.count(method) ? method : "unknown"},
              {"code", code},
              {"server_ms", ms},
              {"time", static_cast<int64_t>(time(nullptr))}};
    auto text = j.dump() + "\n";
    int fd = open((std::filesystem::path(o.profile) / "http-timings.jsonl").c_str(),
                  O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        (void)write(fd, text.data(), text.size());
        close(fd);
    }
} catch (...) {
    // Timing is best effort, including on the malformed-request error path.
}
int envelopeCode(int error) {
    switch (error) {
    case NMW_INVALID:
    case NMW_MESSAGE:
        return 400;
    case NMW_OVERSIZED:
        return 413;
    case NMW_BUSY:
        return 503;
    case NMW_TIMEOUT:
        return 504;
    default:
        return 502;
    }
}
int run(const Options &o) {
    umask(0077);
    std::filesystem::create_directories(o.profile);
    chmod(o.profile.c_str(), 0700);
    Service service(o.binary, o.assets, o.profile);
    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(8); };
    server.set_payload_max_length(maxBody);
    server.set_read_timeout(10, 0);
    server.set_write_timeout(30, 0);
    const std::set<std::string> routes = {"/status", "/m3u8",        "/key",     "/decrypt", "/lyrics",
                                          "/assets", "/webplayback", "/license", "/artist"};
    auto reject = [&](const httplib::Request &req, httplib::Response &res, int code, const char *message) {
        res.status = 200;
        res.set_header("Cache-Control", "no-store");
        res.set_header("Connection", "close");
        res.set_header("X-Wrapper-Backend", "native-macos-arm64");
        res.set_header("Server-Timing", "app;dur=0");
        res.set_content(Json{{"code", code}, {"msg", message}}.dump(), "application/json; charset=utf-8");
        record(o, routes.count(req.path) ? req.path : "unknown", req.method, code, 0);
        return httplib::Server::HandlerResponse::Handled;
    };
    server.set_pre_routing_handler([&](const httplib::Request &req, httplib::Response &res) {
        auto host = req.get_header_value("Host");
        host = host.substr(0, host.find(':'));
        if ((host != "127.0.0.1" && host != "localhost") || req.has_header("Origin"))
            return reject(req, res, 403, "Local clients only");
        if (req.has_header("Transfer-Encoding"))
            return reject(req, res, 400, "Unsupported transfer encoding");
        if (req.method == "POST") {
            if (req.get_header_value_count("Content-Length") != 1)
                return reject(req, res, 400, "Invalid content length");
            auto size = req.get_header_value("Content-Length");
            if (size.empty() || size.size() > 18 || size.find_first_not_of("0123456789") != size.npos)
                return reject(req, res, 400, "Invalid content length");
            if (std::stoull(size) > (req.path == "/decrypt" ? maxBody : maxMetadata))
                return reject(req, res, 413, "Request is too large");
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_error_handler(
        [&](const httplib::Request &req, httplib::Response &res) -> httplib::Server::HandlerResponse {
            return reject(req, res, res.status >= 400 ? res.status : 400, "Invalid wrapper request");
        });
    auto handler = [&](const httplib::Request &req, httplib::Response &res) {
        const auto started = Clock::now();
        int code = 0;
        Reply output;
        std::string route = req.path;
        try {
            auto host = req.get_header_value("Host");
            host = host.substr(0, host.find(':'));
            if ((host != "127.0.0.1" && host != "localhost") || req.has_header("Origin")) {
                code = 403;
                fail(NMW_INVALID, "Local clients only");
            }
            if (!routes.count(route)) {
                code = 404;
                fail(NMW_INVALID, "not found");
            }
            if (req.method != ((route == "/license" || route == "/decrypt") ? "POST" : "GET")) {
                code = 405;
                fail(NMW_INVALID, "method not allowed");
            }
            Json query = Json::object();
            for (const auto &p : req.params) {
                if (query.contains(p.first)) {
                    code = 400;
                    fail(NMW_INVALID, "duplicate query parameter");
                }
                query[p.first] = p.second;
            }
            if (req.has_header("Transfer-Encoding")) {
                code = 400;
                fail(NMW_INVALID, "unsupported transfer encoding");
            }
            if (route == "/decrypt" &&
                req.get_header_value("Content-Type").find("application/octet-stream") != 0) {
                code = 400;
                fail(NMW_INVALID, "invalid binary content type");
            }
            NMWOperation op;
            auto body = Bytes(req.body.begin(), req.body.end());
            output = service.send(route, query, body, req.get_header_value("X-Native-Context"), op);
            code = output.envelope["code"].get<int>();
            if (route == "/status" && code == 0) {
                auto &data = output.envelope["data"];
                data["transport"] = "http";
                data["server_instance"] = o.instance;
            }
            if (code == 0)
                output.envelope["msg"] = "SUCCESS";
        } catch (const Failure &e) {
            if (!code)
                code = envelopeCode(e.code);
            output.envelope = {{"code", code}, {"msg", e.message.empty() ? "Native request failed" : e.message}};
        } catch (...) {
            code = 400;
            output.envelope = {{"code", code}, {"msg", "invalid request"}};
        }
        auto ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        record(o, routes.count(route) ? route : "unknown", req.method, code, ms);
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Wrapper-Backend", "native-macos-arm64");
        res.set_header("Server-Timing", "app;dur=" + std::to_string(ms));
        res.status = 200;
        if (!output.binary.empty() && !code)
            res.set_content(reinterpret_cast<const char *>(output.binary.data()), output.binary.size(),
                            "application/octet-stream");
        else
            res.set_content(output.envelope.dump(), "application/json; charset=utf-8");
    };
    server.Get(R"(/.*)", handler);
    server.Post(R"(/.*)", handler);
    server.Options(R"(/.*)", handler);
    if (!server.bind_to_port(o.host, o.port))
        return 1;
    std::signal(SIGTERM, signalStop);
    std::signal(SIGINT, signalStop);
    std::atomic<bool> done{false};
    std::thread monitor([&] {
        while (!done) {
            if (stopping || (o.owner && getppid() != o.owner)) {
                server.stop();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    bool ok = server.listen_after_bind();
    done = true;
    monitor.join();
    return ok ? 0 : 1;
}
} // namespace
int main(int argc, char **argv) {
    try {
        return run(options(argc, argv));
    } catch (...) {
        std::cerr << "Could not start the native wrapper server.\n";
        return 1;
    }
}
