#include "test_support.h"
#include "../third_party/http/httplib.h"
#include "../device_asset.h"
#include "../music_client.h"
#include <curl/curl.h>
#include <array>
#include <future>
#include <set>
using namespace validation;
using namespace wrapper;
namespace {
struct Fixture {
    std::vector<unsigned> commands;
    std::vector<Json> requests;
    Json asset = {{"adamId", "123"}, {"m3u8", "https://aod.itunes.apple.com/device.m3u8"}, {"streamType", "hls"}};
    Json api = Json::object();
    std::string errorURL;
    int httpError = 0;
    int version = 1;
    uint32_t next = 0;
    bool broken = false;
    bool account = true;
    static int32_t invoke(void *ptr, int32_t kind, const NMWOperation *, const uint8_t *raw, size_t n,
                          NMWBytes *output) {
        auto &f = *static_cast<Fixture *>(ptr);
        Bytes data;
        if (n)
            data.assign(raw, raw + n);
        Bytes out;
        try {
            if (kind == NMW_GENERATION)
                appendWord(out, f.version);
            else if (kind == NMW_CLOSE_WORKER) {
                ++f.version;
                f.next = 0;
            } else if (kind == NMW_HTTP) {
                auto q = json(data);
                f.requests.push_back(q);
                if (q["url"] == f.errorURL) return f.httpError;
                std::string result;
                if (q["url"] == "https://music.apple.com")
                    result = "<script src=\"/assets/index~test.js\"></script>";
                else if (q["url"] == "https://music.apple.com/assets/index~test.js")
                    result = "eyJhbGciOiJIUzI1NiJ9.eyJpc3MiOiJBTVBXZWJQbGF5In0.dGVzdA";
                else
                    result = f.api.dump();
                out.assign(result.begin(), result.end());
            } else if (kind == NMW_WORKER) {
                auto cmd = word(data, 0);
                f.commands.push_back(cmd);
                if (!f.account && (cmd == 1 || cmd == 5 || cmd == 6))
                    return NMW_NO_ACCOUNT;
                if (cmd == 4)
                    out = bytes(Json{{"backend", "macos-arm64"}, {"account_present", true}, {"regions", {"ph"}}});
                else if (cmd == 5)
                    out = bytes(
                        Json{{"storefront", "ph"}, {"developer_token", "sdk"}, {"music_token", "test-user"}});
                else if (cmd == 6)
                    out = bytes(f.asset);
                else if (cmd == 1) {
                    appendWord(out, ++f.next);
                    appendString(out, "opaque-test-key");
                } else if (cmd == 2) {
                    out.assign(data.begin() + 8, data.end());
                    if (f.broken)
                        out.clear();
                } else if (cmd != 3)
                    return NMW_REJECTED;
            }
            *output = nmw_copy_bytes(out.data(), out.size());
            return NMW_OK;
        } catch (...) {
            return NMW_INVALID;
        }
    }
};
struct Env {
    Fixture fixture;
    NMWOperation operation;
    Service service{"", "", "", &fixture, Fixture::invoke};
    Reply send(const std::string &route, Json q = Json::object(), Bytes body = {}, std::string context = {}) {
        return service.send(route, q, body, context, operation);
    }
    Json manifest(Json extra = Json::object()) {
        extra["adamId"] = "123";
        return send("/m3u8", extra).envelope;
    }
    Json key(const std::string &uri = "skd://unit/c6") {
        return send("/key", {{"adamId", "123"}, {"uri", uri}}).envelope.at("data");
    }
};
template <class Fn> void rejects(Fn fn) {
    try {
        fn();
    } catch (const Failure &) {
        return;
    }
    throw std::runtime_error("operation accepted");
}
Bytes samples() {
    Bytes b;
    appendWord(b, 2);
    appendWord(b, 3);
    b.insert(b.end(), {1, 2, 3});
    appendWord(b, 1);
    b.push_back(4);
    return b;
}
void outputs(Report &r) {
    r.run("license DRM types reject supplied invalid values before external work", [] {
        for (const auto &drm : {Json(nullptr), Json(123), Json(true), Json::object(), Json::array(),
                               Json(""), Json("widevine"), Json("WV")}) {
            Env e;
            rejects([&] { e.send("/license", Json::object(), bytes(Json{
                {"adamId", "123"}, {"challenge", "QQ=="}, {"uri", "skd://test/key"}, {"drm-type", drm}})); });
            require(e.fixture.commands.empty() && e.fixture.requests.empty(), "invalid DRM reached external boundary");
        }
        for (auto drm : {"absent", "wv", "pr"}) {
            Env e;
            e.fixture.api = {{"license", "fixture-license"}};
            Json body = {{"adamId", "123"}, {"challenge", "QQ=="}, {"uri", "skd://test/key"}};
            if (std::string(drm) != "absent") body["drm-type"] = drm;
            require(e.send("/license", Json::object(), bytes(body)).envelope["code"] == 0, "valid DRM rejected");
            auto sent = Json::parse(e.fixture.requests.back()["body"].get<std::string>());
            require(sent["key-system"] == (std::string(drm) == "pr" ? "com.microsoft.playready" : "com.widevine.alpha"),
                    "wrong key system");
        }
    });
    r.run("malformed optional hero artwork preserves valid artist metadata", [] {
        Env e;
        e.fixture.api = {{"data", Json::array({Json{{"id", "123"}, {"type", "artists"},
                    {"attributes", {{"name", "Fixture Artist"}}}}})}};
        auto &attributes = e.fixture.api["data"][0]["attributes"];
        for (const auto &content : {Json(nullptr), Json(42), Json("wrong type"), Json::array(), Json::object()}) {
            attributes["hero"] = Json::array({Json{{"content", Json::array({content})}}});
            auto result = e.send("/artist", {{"adamId", "123"}}).envelope;
            require(result["code"] == 0 && result["data"]["name"] == "Fixture Artist" &&
                        result["data"]["wideArtwork"].is_null(), "optional hero rejected artist");
        }
        attributes["hero"] = Json::array({Json{{"content", Json::array({Json{{"artwork",
            {{"width", 100}, {"height", 100}, {"url", "https://is1-ssl.mzstatic.com/image/{w}x{h}bb.jpg"},
             {"recommendedCropCodes", Json::array({"vf"})}}}}})}}});
        auto wide = e.send("/artist", {{"adamId", "123"}}).envelope["data"]["wideArtwork"];
        require(wide["width"] == 1600 && wide["height"] == 900 &&
                    wide["url"] == "https://is1-ssl.mzstatic.com/image/1600x900vf.jpg", "valid hero was lost");
    });
    r.run("artist not found applies only to catalog lookup failures", [] {
        for (auto url : {"https://music.apple.com", "https://music.apple.com/assets/index~test.js"}) {
            Env e;
            e.fixture.errorURL = url;
            e.fixture.httpError = NMW_HTTP_BASE + 404;
            int code = 0;
            try { e.send("/artist", {{"adamId", "123"}}); }
            catch (const Failure &failure) { code = failure.code; }
            require(code == NMW_HTTP_BASE + 404, "bootstrap 404 became artist not found");
        }
        Env e;
        e.fixture.errorURL = "https://amp-api.music.apple.com/v1/catalog/us/artists/123"
                             "?extend=artistBio,origin,bornOrFormed,editorialArtwork,hero";
        e.fixture.httpError = NMW_HTTP_BASE + 404;
        require(e.send("/artist", {{"adamId", "123"}}).envelope["code"] == 404, "catalog 404 lost");
        require(e.fixture.commands.empty(), "artist lookup used account");
    });
    r.run("token-cache waiters cancel independently of an artist refresh", [] {
        struct Fixture { std::promise<void> entered, tokens, release; } fixture;
        auto hook = [](void *context, int32_t kind, const NMWOperation *, const uint8_t *, size_t, NMWBytes *out) -> int32_t {
            auto &f = *static_cast<Fixture *>(context);
            if (kind == NMW_HTTP) {
                f.entered.set_value();
                f.release.get_future().wait();
                return NMW_UNAVAILABLE;
            }
            if (kind != NMW_WORKER) return NMW_INVALID;
            auto data = bytes(Json{{"storefront", "us"}, {"music_token", "user"}, {"developer_token", "sdk"}});
            *out = nmw_copy_bytes(data.data(), data.size());
            f.tokens.set_value();
            return NMW_OK;
        };
        auto runtime = nmw_create("", "", "", &fixture, hook);
        require(runtime != nullptr, "runtime creation");
        auto send = [&](const char *route, NMWOperation &operation) {
            auto reply = nmw_send(runtime, &operation, route, R"({"adamId":"123"})", nullptr, 0, "");
            nmw_free_bytes(reply.metadata); nmw_free_bytes(reply.binary);
            return reply.status;
        };
        NMWOperation artist, lyrics;
        auto owner = std::async(std::launch::async, [&] { return send("/artist", artist); });
        fixture.entered.get_future().wait();
        auto waiter = std::async(std::launch::async, [&] { return send("/lyrics", lyrics); });
        fixture.tokens.get_future().wait();
        bool waiting = waiter.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout;
        nmw_operation_cancel(&lyrics);
        bool prompt = waiter.wait_for(std::chrono::milliseconds(400)) == std::future_status::ready;
        fixture.release.set_value();
        auto status = waiter.get();
        auto ownerStatus = owner.get();
        nmw_destroy(runtime);
        require(waiting && prompt && status == NMW_CANCELLED && ownerStatus == NMW_UNAVAILABLE,
                "cancellation waited for refresh");
    });
    r.run("worker close completes after work exceeding the request admission deadline", [] {
        struct Fixture {
            std::promise<void> entered, release;
            unsigned closes = 0;
        } fixture;
        auto hook = [](void *context, int32_t kind, const NMWOperation *, const uint8_t *, size_t, NMWBytes *out) -> int32_t {
            auto &f = *static_cast<Fixture *>(context);
            if (kind == NMW_CLOSE_WORKER) { ++f.closes; return NMW_OK; }
            if (kind != NMW_WORKER) return NMW_INVALID;
            f.entered.set_value();
            f.release.get_future().wait();
            auto data = bytes(Json{{"account_present", false}});
            *out = nmw_copy_bytes(data.data(), data.size());
            return NMW_OK;
        };
        auto runtime = nmw_create("", "", "", &fixture, hook);
        require(runtime != nullptr, "runtime creation");
        auto active = std::async(std::launch::async, [&] {
            NMWOperation op;
            auto result = nmw_send(runtime, &op, "/status", "{}", nullptr, 0, "");
            nmw_free_bytes(result.metadata); nmw_free_bytes(result.binary);
            return result.status;
        });
        fixture.entered.get_future().wait();
        auto closing = std::async(std::launch::async, [&] { nmw_close_worker(runtime); });
        bool returnedEarly = closing.wait_for(std::chrono::milliseconds(5200)) == std::future_status::ready;
        fixture.release.set_value();
        auto status = active.get();
        closing.get();
        nmw_destroy(runtime);
        require(!returnedEarly && status == NMW_OK && fixture.closes == 1, "worker close silently dropped");
    });
    r.run("authentication and missing-content errors retain their envelope codes", [] {
        Env e;
        e.fixture.account = false;
        require(e.manifest()["code"] == 401, "missing account code");
        e.fixture.account = true;
        e.fixture.api = {{"data", Json::array()}};
        require(e.send("/lyrics", {{"adamId", "123"}}).envelope["code"] == 404, "empty lyrics code");
        e.fixture.api = {{"data", Json::array({Json{{"attributes", Json::object()}}})}};
        require(e.send("/lyrics", {{"adamId", "123"}}).envelope["code"] == 404, "absent lyrics code");
        e.fixture.api = {{"songList", Json::array()}};
        require(e.send("/webplayback", {{"adamId", "123"}}).envelope["code"] == 404, "empty playback code");
    });
    r.run("failed playback leases remain retryable and successful leases are reused", [] {
        native_poc::DeviceManifestSession session;
        unsigned calls = 0;
        auto request = [](void *context, const bool *interrupt) {
            require(!*interrupt, "lease interrupted other devices");
            return ++*static_cast<unsigned *>(context) > 1;
        };
        bool rejected = false;
        try { session.requirePlaybackLease(request, &calls); }
        catch (const std::runtime_error &) { rejected = true; }
        require(rejected && !session.requestedPlaybackLease, "failed lease was cached");
        session.requirePlaybackLease(request, &calls);
        session.requirePlaybackLease(request, &calls);
        require(session.requestedPlaybackLease && calls == 2, "lease retry or reuse");
    });
    r.run("device lookup order for every entitlement and source combination", [] {
        for (bool allowed : {false, true})
            for (bool download : {false, true})
                for (bool online : {false, true}) {
                    std::string calls;
                    auto chosen = native_poc::resolveDeviceAsset(
                        allowed,
                        [&] {
                            calls += "S";
                            return native_poc::DeviceAsset{download ? "file.m4p" : "", "LWHQ"};
                        },
                        [&] {
                            calls += "P";
                            return native_poc::DeviceAsset{online ? "stream.m3u8" : "", "HLS"};
                        });
                    require(calls == (allowed ? (download ? "S" : "SP") : (online ? "P" : "PS")), "order");
                    require(chosen.url == (allowed ? (download ? "file.m4p"
                                                      : online ? "stream.m3u8"
                                                               : "")
                                                   : (online     ? "stream.m3u8"
                                                      : download ? "file.m4p"
                                                                 : "")),
                            "asset");
                }
    });
    r.run("device HLS reaches playback without catalog, tokens, or web services", [] {
        Env e;
        auto p = e.manifest({{"representation", "hls"}});
        require(p["data"] == e.fixture.asset && e.fixture.commands == std::vector<unsigned>{6} &&
                    e.fixture.requests.empty(),
                "device reply");
    });
    r.run("direct file stays unmodified in the original contract", [] {
        Env e;
        e.fixture.asset["m3u8"] = "https://streamingaudio.itunes.apple.com/test.m4p";
        e.fixture.asset["streamType"] = "file";
        require(e.manifest()["data"] == e.fixture.asset && e.fixture.requests.empty(), "direct response");
    });
    r.run("authenticated CBC stream only for a device-returned file", [] {
        Env e;
        e.fixture.asset["streamType"] = "file";
        e.fixture.api = {
            {"results", {{"assets", Json::array({Json{{"url", "https://aod.itunes.apple.com/aac.m3u8"}}})}}}};
        auto d = e.manifest({{"representation", "hls"}})["data"];
        require(d["streamType"] == "hls" && d["m3u8"] == "https://aod.itunes.apple.com/aac.m3u8",
                "streamable output");
        auto req = e.fixture.requests.back();
        require(field(req, "url").find("/v1/play/assets?") != std::string::npos &&
                    req["headers"]["User-Agent"] == native_poc::music_client::userAgent(),
                "device and endpoint");
    });
    r.run("missing device asset gives 404 with zero other lookups", [] {
        Env e;
        e.fixture.asset["m3u8"] = nullptr;
        require(e.manifest({{"representation", "hls"}})["code"] == 404 && e.fixture.requests.empty(),
                "no source substitution");
    });
    r.run("wrong ID or unsafe native asset is rejected", [] {
        Env e;
        e.fixture.asset["adamId"] = "456";
        rejects([&] { e.manifest(); });
        e.fixture.asset["adamId"] = "123";
        e.fixture.asset["m3u8"] = "https://example.invalid/a";
        rejects([&] { e.manifest(); });
    });
    r.run("invalid input rejected before device access", [] {
        Env e;
        for (auto s : {"", "0", "-1", "1&other=1", "9999999999999999999"})
            rejects([&] { e.send("/m3u8", {{"adamId", s}}); });
        rejects([&] { e.key("skd://itunes.apple.com/P000000000/s1/e1"); });
        require(e.fixture.commands.empty(), "worker reached");
    });
    r.run("key lookup reuse, separated rendition keys, and worker restart", [] {
        Env e;
        auto a = e.key(), b = e.key();
        require(a == b, "same descriptor");
        require(e.key("skd://unit/c7")["nativeContext"] != a["nativeContext"], "distinct keys");
        e.service.closeWorker();
        auto clear = e.send("/decrypt", Json::object(), samples(), a["nativeContext"]);
        require(clear.binary == samples(), "reopened sample layout");
        require(std::count(e.fixture.commands.begin(), e.fixture.commands.end(), 1) == 3, "key reopen count");
    });
    r.run("expired or evicted native contexts return 409", [] {
        Env e;
        auto first = e.key();
        for (int i = 0; i < 34; ++i)
            e.key("skd://test/" + std::to_string(i));
        auto before = e.fixture.commands.size();
        require(e.send("/decrypt", Json::object(), samples(), first["nativeContext"]).envelope["code"] == 409 &&
                    before == e.fixture.commands.size(),
                "expiry");
    });
    r.run("malformed sample batches cannot reach the SDK", [] {
        Env e;
        for (auto b : {Bytes{}, Bytes{0, 0, 0, 0}, Bytes{0, 0, 0, 1, 0, 0, 0, 2, 0}})
            rejects([&] { e.send("/decrypt", Json::object(), b, std::string(48, 'a')); });
        require(e.fixture.commands.empty(), "invalid sample forwarded");
    });
    r.run("corrupted native clear batch cannot be published", [] {
        Env e;
        auto key = e.key();
        e.fixture.broken = true;
        rejects([&] { e.send("/decrypt", Json::object(), samples(), key["nativeContext"]); });
        require(e.fixture.version > 1, "worker not reset");
    });
    r.run("native worker recycling retains a bounded context table", [] {
        Env e;
        for (int i = 0; i < 65; ++i)
            e.key("skd://test/" + std::to_string(i));
        require(e.fixture.version == 2 && e.fixture.next == 1, "recycle");
    });
    r.run("plain or timed lyrics and translation keys", [] {
        Env e;
        e.fixture.api = {
            {"data",
             Json::array({Json{{"attributes",
                                {{"ttmlLocalizations", "<tt itunes:timing=\"Word\"><p><span begin=\"1s\">A</span> "
                                                       " B</p><text itunes:key=\"k\">C</text></tt>"}}}}})}};
        auto q = Json{{"adamId", "123"}, {"syllable", "0"}};
        auto v = field(e.send("/lyrics", q).envelope["data"], "lyrics");
        require(v.find("timing=\"Line\"") != v.npos && v.find("<p>A B</p>") != v.npos &&
                    v.find("itunes:key") != v.npos,
                "line conversion");
        e.fixture.api["data"][0]["attributes"] = {{"text", "plain fixture"}};
        require(e.send("/lyrics", q).envelope["data"]["lyrics"] == "plain fixture", "plain fallback");
    });
    r.run("valid Apple result can carry an empty errors array", [] {
        Env e;
        e.fixture.api = {
            {"errors", Json::array()},
            {"songList",
             Json::array({Json{{"songId", "123"}, {"hls-playlist-url", "https://audio.apple.com/master.m3u8"}}})}};
        require(e.send("/webplayback", {{"adamId", "123"}}).envelope["data"]["streamType"] == "hls",
                "empty errors");
    });
    r.run("bad license challenge never contacts Apple", [] {
        Env e;
        rejects([&] {
            e.send("/license", Json::object(),
                   bytes(Json{{"adamId", "123"}, {"challenge", "!"}, {"uri", "skd://test"}}));
        });
        require(e.fixture.requests.empty() && e.fixture.commands.empty(), "license reached service");
    });
    r.run("HTTPS redirects keep the exact origin", [] {
        require(sameOrigin("https://music.apple.com/start", "https://music.apple.com/next"), "valid redirect");
        for (auto target : {"http://music.apple.com/a", "https://evil.example/a", "https://music.apple.com:444/a",
                            "https://x@music.apple.com/a"})
            require(!sameOrigin("https://music.apple.com/a", target), "unsafe redirect");
    });
    r.run("unsupported explicit TLS mode retries the platform default before transfer only", [] {
        require(mayRetryTLSWithSystemDefault(CURLE_NOT_BUILT_IN, 0), "system libcurl feature failure");
        require(mayRetryTLSWithSystemDefault(CURLE_SSL_CONNECT_ERROR, 0), "handshake reset");
        require(!mayRetryTLSWithSystemDefault(CURLE_PEER_FAILED_VERIFICATION, 0), "certificate failure was retried");
        require(!mayRetryTLSWithSystemDefault(CURLE_SSL_CONNECT_ERROR, 1), "partial response was retried");
    });
    r.run("decompressed deliveries share a single bounded response buffer", [] {
        Bytes body;
        const std::string chunk(33, 'a');
        require(appendBounded(body, chunk.data(), 32, 64), "first delivery");
        require(appendBounded(body, chunk.data(), 32, 64), "second delivery");
        require(!appendBounded(body, chunk.data(), 1, 64) && body.size() == 64, "limit exceeded");
        Bytes other;
        require(!appendBounded(other, chunk.data(), SIZE_MAX, 64) && other.empty(), "input overflow");
    });
    r.run("account operation excludes playback and refreshes credentials", [] {
        Env e;
        e.key();
        e.service.beginAccount(e.operation);
        rejects([&] { e.manifest(); });
        e.service.endAccount();
        require(e.manifest()["code"] == 0, "resume");
    });
    r.run("gate cancellation and capacity", [] {
        Gate gate;
        NMWOperation first;
        gate.acquire(first);
        std::array<NMWOperation, 7> ops;
        std::vector<std::future<int>> tasks;
        for (auto &op : ops)
            tasks.push_back(std::async(std::launch::async, [&gate, &op] {
                try {
                    Admission a(gate, op);
                    return static_cast<int>(NMW_OK);
                } catch (const Failure &e) {
                    return static_cast<int>(e.code);
                }
            }));
        auto deadline = Clock::now() + std::chrono::seconds(2);
        while (gate.waiting() != 7 && Clock::now() < deadline)
            std::this_thread::yield();
        bool full = false;
        NMWOperation extra;
        try {
            gate.acquire(extra);
        } catch (const Failure &e) {
            full = e.code == NMW_BUSY;
        }
        ops[3].cancelled = true;
        auto cancelled = tasks[3].get();
        gate.release();
        for (size_t i = 0; i < tasks.size(); ++i)
            if (i != 3)
                require(tasks[i].get() == 0, "queued request failed");
        require(full && cancelled == NMW_CANCELLED, "limit or cancellation");
    });
}
} // namespace
void loaderTests(Report &, const fs::path &, const fs::path &, const fs::path &);
void standaloneTests(Report &, const fs::path &, const fs::path &);
void assetTests(Report &, const fs::path &, const fs::path &);
void profileTests(Report &);
int main(int argc, char **argv) {
    if (argc == 3 && std::string(argv[1]) == "--inheritance-fixture") {
        std::string input;
        std::getline(std::cin, input);
        std::cout << Json{{"input", input},
                          {"descriptor", fcntl(std::stoi(argv[2]), F_GETFD) >= 0},
                          {"environment", std::getenv("WRAPPER_TEST_PARENT_SECRET") != nullptr},
                          {"path", std::getenv("PATH") ? std::getenv("PATH") : ""}}.dump();
        return 0;
    }
    if (argc != 4)
        return 2;
    Report r;
    outputs(r);
    profileTests(r);
    fs::path build = fs::absolute(argv[1]), assets = fs::absolute(argv[2]), fixture = fs::absolute(argv[3]);
    loaderTests(r, build / "wrapper-native-poc", assets, fixture);
    standaloneTests(r, build, assets);
    assetTests(r, build, assets);
    std::cout << r.passed << " passed, " << r.failed << " failed\n";
    return r.failed ? 1 : 0;
}
