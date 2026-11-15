#include "test_support.h"
#include "../third_party/http/httplib.h"
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
using namespace validation;
namespace {
int freePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(fd >= 0 && !bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)), "port");
    socklen_t n = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n);
    close(fd);
    return ntohs(a.sin_port);
}
Json envelope(const httplib::Result &r) {
    require(r && r->status == 200, "HTTP envelope");
    return Json::parse(r->body);
}
int runAsset(const fs::path &tool, const fs::path &src, const fs::path &dst) {
    Temp t;
    Process p({tool.string(), src.string(), dst.string()}, t.path);
    return p.wait();
}
std::string rawRequest(int port, const std::string &request) {
    struct Socket {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        ~Socket() { if (fd >= 0) close(fd); }
    } connection;
    require(connection.fd >= 0, "socket");
    timeval timeout{3, 0};
    int noSignal = 1;
    setsockopt(connection.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(connection.fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    require(!connect(connection.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), "connect");
    size_t sent = 0;
    while (sent < request.size()) {
        auto n = send(connection.fd, request.data() + sent, request.size() - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0, "send request");
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buffer[4096];
    for (;;) {
        auto n = recv(connection.fd, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) continue;
        if (!n || (n < 0 && errno == ECONNRESET)) break;
        require(n > 0, "response did not close");
        response.append(buffer, static_cast<size_t>(n));
    }
    return response;
}
} // namespace
void standaloneTests(Report &r, const fs::path &build, const fs::path &assets) {
    r.run("private child inherits only its pipes and selected environment", [&] {
        struct ParentState {
            int fd = -1;
            std::string previous;
            bool hadPrevious = false;
            ParentState() {
                if (auto value = std::getenv("WRAPPER_TEST_PARENT_SECRET")) {
                    previous = value;
                    hadPrevious = true;
                }
                setenv("WRAPPER_TEST_PARENT_SECRET", "synthetic-secret", 1);
                fd = fcntl(STDOUT_FILENO, F_DUPFD, 100);
            }
            ~ParentState() {
                if (fd >= 0) close(fd);
                if (hadPrevious) setenv("WRAPPER_TEST_PARENT_SECRET", previous.c_str(), 1);
                else unsetenv("WRAPPER_TEST_PARENT_SECRET");
            }
        } parent;
        require(parent.fd >= 0, "inheritable fixture descriptor");
        wrapper::PrivateChild child;
        child.spawn({(build / "native-wrapper-tests").string(), "--inheritance-fixture", std::to_string(parent.fd)});
        NMWOperation operation;
        auto deadline = wrapper::Clock::now() + std::chrono::seconds(3);
        const std::string input = "private pipe\n";
        wrapper::pipeWrite(child.input, reinterpret_cast<const uint8_t *>(input.data()), input.size(), operation, deadline);
        std::string result;
        uint8_t buffer[1024];
        for (;;) {
            auto count = wrapper::pipeReadSome(child.output, buffer, sizeof(buffer), operation, deadline);
            if (!count) break;
            result.append(reinterpret_cast<const char *>(buffer), count);
        }
        auto data = Json::parse(result);
        require(data["input"] == "private pipe" && data["descriptor"] == false &&
                    data["environment"] == false && data["path"] == "/usr/bin:/bin:/usr/sbin:/sbin",
                "unexpected child inheritance");
    });
    Temp t;
    auto profile = t.path / "service";
    auto port = freePort();
    const std::string instance = "cpp-test-instance";
    Process server({(build / "native-music-wrapper-server").string(), "--binary",
                    (build / "wrapper-native-poc").string(), "--assets", assets.string(), "--data-dir",
                    profile.string(), "--port", std::to_string(port), "--instance-id", instance},
                   t.path);
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(1);
    client.set_read_timeout(5);
    auto deadline = wrapper::Clock::now() + std::chrono::seconds(8);
    while (wrapper::Clock::now() < deadline) {
        if (client.Get("/status"))
            break;
        usleep(15000);
    }
    r.run("standalone status identifies this native process and saved account", [&] {
        auto response = client.Get("/status");
        auto j = envelope(response);
        require(j["code"] == 0 && j["data"]["backend"] == "macos-arm64" &&
                    j["data"]["server_instance"] == instance && j["data"]["account_present"] == false,
                "wrong server");
        require(response->get_header_value("Cache-Control") == "no-store" && response->has_header("Server-Timing"),
                "private timing headers");
    });
    r.run("standalone rejects cross-origin and off-host clients", [&] {
        for (const httplib::Headers &h : {httplib::Headers{{"Origin", "https://example.invalid"}},
                                          httplib::Headers{{"Host", "public.example"}}})
            require(envelope(client.Get("/status", h))["code"] == 403, "unexpectedly accepted client");
    });
    r.run("early rejection closes before an unread body can become a request", [&] {
        const std::string inner = "GET /status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        auto header = [](size_t size) {
            return "POST /license HTTP/1.1\r\nHost: localhost\r\nOrigin: https://example.invalid\r\n"
                   "Content-Length: " + std::to_string(size) + "\r\n\r\n";
        };
        size_t size = 4096 + inner.size();
        for (int i = 0; i < 4; ++i) size = 4096 - header(size).size() + inner.size();
        auto prefix = header(size);
        auto response = rawRequest(port, prefix + std::string(4096 - prefix.size(), 'A') + inner);
        require(response.find("\"code\":403") != response.npos, "missing origin rejection");
        require(response.find("HTTP/1.1 ", 1) == response.npos &&
                    response.find("server_instance") == response.npos, "body was dispatched");
    });
    r.run("standalone rejects unknown routes, wrong method, duplicate parameters and invalid JSON", [&] {
        require(envelope(client.Get("/unknown"))["code"] == 404, "unknown endpoint");
        require(envelope(client.Post("/key", "", "application/json"))["code"] == 405, "method");
        require(envelope(client.Get("/m3u8?adamId=1&adamId=2"))["code"] == 400, "duplicate");
        require(envelope(client.Post("/license", "{", "application/json"))["code"] == 400, "bad JSON");
    });
    r.run("unsupported methods and routes are rejected before reading a body", [&] {
        for (auto method : {"PUT", "PATCH", "DELETE"}) {
            auto response = rawRequest(port, std::string(method) + " /status HTTP/1.1\r\nHost: localhost\r\n\r\n");
            require(response.find("\"code\":405") != response.npos, "lengthless method read its body");
        }
        auto unknown = rawRequest(port, "POST /unknown HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999999999\r\n\r\n");
        require(unknown.find("\"code\":404") != unknown.npos, "unknown route read its body");
        auto get = rawRequest(port, "GET /status HTTP/1.1\r\nHost: localhost\r\nContent-Length: 20\r\n\r\n");
        require(get.find("\"code\":400") != get.npos, "GET accepted an unread body");
    });
    r.run("invalid binary and identity input never succeeds", [&] {
        require(envelope(client.Post("/decrypt", "abc", "application/json"))["code"] == 400, "type");
        require(envelope(client.Get("/m3u8?adamId=-1"))["code"] == 400, "ID");
    });
    r.run("malformed methods cannot crash or inject data into timing logs", [&] {
        for (const auto &method : {std::string(1, char(0xff)), std::string("private-method-fixture")}) {
            auto response = rawRequest(port, method + " /status HTTP/1.1\r\nHost: localhost\r\n\r\n");
            require(response.find("\"code\":400") != response.npos, "malformed method response");
            require(envelope(client.Get("/status"))["code"] == 0, "server stopped");
        }
        auto log = read(profile / "http-timings.jsonl");
        require(log.find("private-method-fixture") == log.npos && log.find(char(0xff)) == log.npos,
                "raw method was logged");
        require(log.find("\"method\":\"unknown\"") != log.npos, "missing stable method");
    });
    r.run("timings contain route and duration without secrets or signed URLs", [&] {
        (void)client.Get("/unknown?secret=fixture-secret");
        auto log = read(profile / "http-timings.jsonl");
        require(log.find("fixture-secret") == std::string::npos && log.find("server_ms") != std::string::npos,
                "unsafe timings");
        struct stat st{};
        require(!stat((profile / "http-timings.jsonl").c_str(), &st) && (st.st_mode & 077) == 0, "permissions");
    });
    kill(server.pid, SIGTERM);
    r.run("standalone shuts down and releases the owned worker",
          [&] { require(server.wait(8) == 0, "shutdown"); });
    r.run("untrusted TLS is rejected by the native SDK and the C++ web client", [&] {
        Temp tls;
        auto key = tls.path / "key.pem", cert = tls.path / "cert.pem";
        Process generate({"/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                          "-subj", "/CN=localhost", "-keyout", key.string(), "-out", cert.string()},
                         tls.path);
        require(generate.wait() == 0, "temporary certificate");
        int testPort = freePort();
        std::string address = "127.0.0.1:" + std::to_string(testPort);
        Process listener({"/usr/bin/openssl", "s_server", "-quiet", "-accept", std::to_string(testPort), "-cert",
                          cert.string(), "-key", key.string(), "-www"},
                         tls.path);
        auto until = wrapper::Clock::now() + std::chrono::seconds(4);
        bool ready = false;
        while (wrapper::Clock::now() < until) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(testPort);
            int rc = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
            close(fd);
            if (!rc) {
                ready = true;
                break;
            }
            usleep(15000);
        }
        require(ready, "self-signed TLS listener did not start");
        auto url = "https://" + address + "/";
        write(tls.path / "network-test-url", url);
        Temp output;
        Process sdk({(build / "wrapper-native-poc").string(), "--assets", tls.path.string(), "--apple-profile",
                     "--probe", "network-abi"},
                    output.path);
        require(sdk.wait() == 0 && Json::parse(read(sdk.out))["result"]["untrusted_tls_rejected"] == true,
                "SDK accepted untrusted TLS");
        NMWOperation op;
        bool rejected = false;
        try {
            wrapper::https({url, {}, {}}, op);
        } catch (const wrapper::Failure &) {
            rejected = true;
        }
        require(rejected, "C++ HTTPS accepted untrusted certificate");
    });
}
void assetTests(Report &r, const fs::path &build, const fs::path &src) {
    auto tool = build / "native-asset-tool";
    r.run("stage only pinned ARM64 libraries from a plain source folder", [&] {
        Temp t;
        auto copy = t.path / "plain";
        fs::copy(src, copy, fs::copy_options::recursive);
        write(copy / "extra.so", "unrelated");
        auto dest = t.path / "dest";
        require(runAsset(tool, copy, dest) == 0, "stage");
        require(!fs::exists(dest / "extra.so") && Json::parse(read(dest / "manifest.json"))["files"].size() == 12,
                "wrong inventory");
    });
    for (auto mode : {"missing", "corrupt", "wrong CPU", "unknown entry", "duplicate", "traversal"})
        r.run(std::string("reject invalid SDK input: ") + mode, [&] {
            Temp t;
            auto s = t.path / "source", d = t.path / "dest";
            fs::copy(src, s, fs::copy_options::recursive);
            fs::create_directory(d);
            write(d / "manifest.json", "existing");
            auto manifest = Json::parse(read(s / "manifest.json"));
            auto name = manifest["files"][0]["library"].get<std::string>();
            if (std::string(mode) == "missing")
                fs::remove(s / name);
            else if (std::string(mode) == "corrupt" || std::string(mode) == "wrong CPU") {
                auto bytes = read(s / name);
                bytes[std::string(mode) == "wrong CPU" ? 18 : 70] ^= 42;
                write(s / name, bytes);
                if (std::string(mode) == "wrong CPU") {
                    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
                    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
                    std::string h;
                    for (auto c : digest) {
                        h += "0123456789abcdef"[c >> 4];
                        h += "0123456789abcdef"[c & 15];
                    }
                    manifest["files"][0]["sha256"] = h;
                    write(s / "manifest.json", manifest.dump());
                }
            } else {
                auto x = manifest["files"][0];
                if (std::string(mode) != "duplicate")
                    x["library"] = std::string(mode) == "traversal" ? "../bad.so" : "libz.so";
                manifest["files"].push_back(x);
                write(s / "manifest.json", manifest.dump());
            }
            require(runAsset(tool, s, d) != 0 && read(d / "manifest.json") == "existing", "unsafe replacement");
        });
    r.run("prune only old unmodified owned assets", [&] {
        Temp t;
        auto d = t.path / "dest";
        require(!runAsset(tool, src, d), "stage");
        auto m = Json::parse(read(d / "manifest.json"));
        auto row = m["files"][0];
        auto name = row["library"].get<std::string>();
        fs::copy_file(d / name, d / "retired.so");
        row["library"] = "retired.so";
        m["files"].push_back(row);
        row["library"] = "changed.so";
        m["files"].push_back(row);
        write(d / "changed.so", "caller data");
        write(d / "unrelated.so", "caller data");
        write(d / "manifest.json", m.dump());
        require(!runAsset(tool, src, d) && !fs::exists(d / "retired.so") &&
                    read(d / "changed.so") == "caller data" && read(d / "unrelated.so") == "caller data",
                "unsafe pruning");
    });
}
