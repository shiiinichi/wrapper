#include "../kit/runtime.h"
#include "../third_party/http/httplib.h"
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace wrapper;
int main(int argc, char **argv) {
    try {
        std::string id, base = "http://127.0.0.1:12341";
        fs::path report = "build/native-poc/endpoint-checks/native-http-report.json";
        for (int i = 1; i < argc; ++i) {
            std::string opt = argv[i];
            if (++i >= argc)
                throw std::runtime_error("missing option");
            if (opt == "--adam-id")
                id = argv[i];
            else if (opt == "--base-url")
                base = argv[i];
            else if (opt == "--report")
                report = argv[i];
            else
                throw std::runtime_error("invalid option");
        }
        if (!validID(id) || (base.rfind("http://127.0.0.1:", 0) != 0 && base.rfind("http://localhost:", 0) != 0))
            throw std::runtime_error("use a loopback HTTP URL and numeric adam-id");
        auto part = base.substr(base.rfind(':') + 1);
        if (part.empty() || part.size() > 5 || part.find_first_not_of("0123456789") != part.npos)
            throw std::runtime_error("invalid loopback port");
        int port = std::stoi(part);
        if (port < 1 || port > 65535)
            throw std::runtime_error("invalid loopback port");
        httplib::Client client("127.0.0.1", port);
        client.set_read_timeout(90);
        Json checks = Json::array();
        bool passed = true;
        auto emit = [&](Json r) {
            if (r.contains("passed") && !r["passed"].get<bool>())
                passed = false;
            checks.push_back(r);
            std::cout << r.dump() << "\n";
        };
        auto request = [&](const std::string &name, const std::string &route, httplib::Params query, int expected,
                           const std::string &post = std::string{}) {
            auto before = Clock::now();
            auto path = httplib::append_query_params(route, query);
            auto response = post.empty() ? client.Get(path) : client.Post(path, post, "application/json");
            Json r = {{"case", name}, {"endpoint", route}, {"expected_code", expected}, {"passed", false}};
            Json data;
            if (response && response->status == 200 &&
                response->get_header_value("X-Wrapper-Backend") == "native-macos-arm64" &&
                response->body.size() < 4 * 1024 * 1024) {
                auto payload = Json::parse(response->body, nullptr, false);
                if (payload.is_object()) {
                    r["code"] = payload.value("code", -1);
                    r["passed"] = r["code"] == expected;
                    if (payload.contains("data"))
                        data = payload["data"];
                    r["response_bytes"] = response->body.size();
                }
            }
            r["client_ms"] = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
            emit(r);
            return data;
        };
        auto playlist = [&](const std::string &name, const Json &data) {
            std::string key;
            bool valid = false;
            try {
                auto url = field(data, "m3u8");
                NMWOperation op;
                if (urlHost(url, true).empty())
                    throw std::runtime_error("invalid origin");
                if (field(data, "streamType") == "file" || field(data, "streamType") == "aac-cenc") {
                    emit({{"case", name}, {"passed", true}, {"asset_type", field(data, "streamType")}});
                    return key;
                }
                auto raw = https({url, {}, {}}, op);
                std::string master(raw.begin(), raw.end());
                valid = master.rfind("#EXTM3U", 0) == 0;
                size_t at = 0;
                while ((at = master.find("URI=\"skd://", at)) != master.npos) {
                    auto b = at + 5, end = master.find('"', b);
                    if (end == master.npos)
                        break;
                    auto candidate = master.substr(b, end - b);
                    if (candidate != "skd://itunes.apple.com/P000000000/s1/e1") {
                        key = candidate;
                        break;
                    }
                    at = end + 1;
                }
            } catch (...) {
            }
            emit({{"case", name}, {"passed", valid}, {"track_key_present", !key.empty()}});
            return key;
        };
        auto status = request("status", "/status", {}, 0);
        if (field(status, "backend") != "macos-arm64")
            return 1;
        httplib::Params song = {{"adamId", id}};
        auto asset = request("m3u8", "/m3u8", song, 0);
        auto key = playlist("device_asset", asset);
        for (auto mode : {"1", "0"})
            request(std::string("lyrics_") + mode, "/lyrics", {{"adamId", id}, {"syllable", mode}}, 0);
        playlist("web_playback_asset", request("webplayback", "/webplayback", song, 0));
        if (!key.empty())
            request("track_key", "/key", {{"adamId", id}, {"uri", key}}, 0);
        else
            emit({{"case", "track_key"},
                  {"tested", false},
                  {"reason", "No track-specific key URI in the returned master"}});
        for (auto path : {"/m3u8", "/key", "/lyrics", "/webplayback"})
            request(std::string(path + 1) + "_missing_id", path, {}, 400);
        request("missing_key_uri", "/key", song, 400);
        request("missing_license_fields", "/license", {}, 400, "{}");
        emit({{"case", "license_acquisition"},
              {"tested", false},
              {"reason", "A real DRM client challenge is required"}});
        auto text = Json{{"adamId", id}, {"base_url", base}, {"checks", checks}}.dump(2) + "\n";
        umask(0077);
        if (!report.parent_path().empty())
            fs::create_directories(report.parent_path());
        int fd = open(report.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
            throw std::runtime_error("report could not be saved");
        size_t at = 0;
        while (at < text.size()) {
            auto n = write(fd, text.data() + at, text.size() - at);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                close(fd);
                throw std::runtime_error("report write failed");
            }
            at += n;
        }
        close(fd);
        std::cout << "Response summary: " << report << "\n";
        return passed ? 0 : 1;
    } catch (...) {
        std::cerr
            << "Usage: native-endpoint-check --adam-id ID [--base-url http://127.0.0.1:PORT] [--report PATH]\n";
        return 2;
    }
}
