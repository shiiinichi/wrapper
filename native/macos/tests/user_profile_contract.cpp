#include "test_support.h"
#include "../profile_metadata.h"
#include "../worker_protocol.h"
#include "../third_party/http/httplib.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <future>
#include <sqlite3.h>
using namespace validation;
using namespace wrapper;
namespace {
struct Runtime {
    NMWRuntime* value;
    Runtime(const fs::path& binary = {}, const fs::path& assets = {}, const fs::path& profile = {},
            void* context = nullptr, NMWExternal hook = nullptr)
        : value(nmw_create(binary.c_str(), assets.c_str(), profile.c_str(), context, hook)) {
        require(value != nullptr, "profile runtime");
    }
    ~Runtime() { nmw_destroy(value); }
    struct Result { int status; Json envelope; };
    Result send(const char* route = "/profile", const char* query = "{}", NMWOperation* operation = nullptr) {
        NMWOperation local;
        auto result = nmw_send(value, operation ? operation : &local, route, query, nullptr, 0, "");
        auto data = result.metadata.size ? Json::parse(result.metadata.bytes, result.metadata.bytes + result.metadata.size) : Json();
        nmw_free_bytes(result.metadata);
        nmw_free_bytes(result.binary);
        return {result.status, data};
    }
};
struct Boundary {
    Bytes output = bytes(Json{{"profile_present", true}, {"name", "Renée \"quoted\" 山田 🎵"},
        {"handle", "fixture.handle"}, {"image", {{"url", "https://is1-ssl.mzstatic.com/fixture/{w}x{h}bb.{f}"},
                                             {"width", 512}, {"height", 512}}}});
    std::vector<uint32_t> commands;
    int error = NMW_OK;
    static int32_t invoke(void* context, int32_t kind, const NMWOperation*, const uint8_t* input, size_t size,
                          NMWBytes* result) {
        auto& fixture = *static_cast<Boundary*>(context);
        if (kind == NMW_CLOSE_WORKER) return NMW_OK;
        if (kind != NMW_WORKER || size != 4) return NMW_INVALID;
        auto command = word(Bytes(input, input + size), 0);
        fixture.commands.push_back(command);
        if (command == 4) {
            auto data = bytes(Json{{"account_present", true}});
            *result = nmw_copy_bytes(data.data(), data.size());
        } else if (command == native_poc::profileReadOperation || command == native_poc::profileRefreshOperation) {
            if (fixture.error) return fixture.error;
            *result = nmw_copy_bytes(fixture.output.data(), fixture.output.size());
        } else return NMW_INVALID;
        return NMW_OK;
    }
};
struct Database {
    sqlite3* value = nullptr;
    explicit Database(const fs::path& profile) {
        require(sqlite3_open((profile / "mpl_db/accounts.sqlitedb").c_str(), &value) == SQLITE_OK, "profile database");
    }
    ~Database() { sqlite3_close(value); }
    void execute(const char* sql) {
        require(sqlite3_exec(value, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "profile fixture SQL");
    }
    void profile(int64_t id, const std::string& name, const std::string& handle, const std::string& image) {
        sqlite3_stmt* statement = nullptr;
        require(sqlite3_prepare_v2(value, "INSERT OR REPLACE INTO profile(entityId,name,handle,profileImage,bio,socialProfileId)"
                                         "VALUES(?,?,?,?, 'private biography', 'private-social-id')", -1, &statement, nullptr) == SQLITE_OK,
                "profile fixture statement");
        sqlite3_bind_int64(statement, 1, id);
        sqlite3_bind_text(statement, 2, name.data(), name.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, handle.data(), handle.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, image.data(), image.size(), SQLITE_TRANSIENT);
        int code = sqlite3_step(statement);
        sqlite3_finalize(statement);
        require(code == SQLITE_DONE, "profile fixture insert");
    }
};
void initialize(Runtime& runtime) {
    require(runtime.send("/status").status == NMW_OK, "profile bootstrap");
    nmw_close_worker(runtime.value);
}
const std::string name = "Renée \"quoted\" 山田 🎵";
const Json image = {{"url", "https://is1-ssl.mzstatic.com/fixture/{w}x{h}{c}.{f}"}, {"width", 512}, {"height", 512}};
int freePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(fd >= 0 && !bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), "profile HTTP port");
    socklen_t size = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    close(fd);
    return ntohs(address.sin_port);
}
} // namespace

// Only used as a child executable by the private-pipe cancellation/timeout tests.
int profileWorkerFixture(const fs::path& profile) {
    const uint8_t ready[] = {0, 0, 0, 5, 0, 'N', 'A', 'D', '2'};
    std::cout.write(reinterpret_cast<const char*>(ready), sizeof(ready));
    std::cout.flush();
    char request[8];
    std::cin.read(request, sizeof(request));
    if (!std::cin) return 2;
    write(profile / "entered", "");
    if (!fs::exists(profile / "resume"))
        for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto data = bytes(Json{{"profile_present", false}});
    Bytes frame;
    appendWord(frame, static_cast<uint32_t>(data.size() + 1));
    frame.push_back(0);
    frame.insert(frame.end(), data.begin(), data.end());
    std::cout.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    std::cout.flush();
    return 0;
}

void userProfileTests(Report& r, const fs::path& build, const fs::path& assets) {
    r.run("profile dispatch is bounded, strips private fields and refreshes only explicitly", [] {
        Boundary fixture;
        auto data = json(fixture.output);
        data["dsid"] = "private-account-id";
        data["token"] = "private-token";
        data["image"]["secret"] = "private-image-field";
        fixture.output = bytes(data);
        Runtime runtime({}, {}, {}, &fixture, Boundary::invoke);
        auto result = runtime.send();
        require(result.status == NMW_OK && result.envelope["code"] == 0 &&
                    result.envelope["data"].size() == 4 && result.envelope["data"]["name"] == name &&
                    result.envelope["data"]["image"].size() == 3 &&
                    result.envelope.dump().find("private-") == std::string::npos, "profile projection");
        require(runtime.send("/profile", R"({"refresh":"false"})").status == NMW_OK &&
                    runtime.send("/profile", R"({"refresh":"true"})").status == NMW_OK &&
                    runtime.send("/status").status == NMW_OK &&
                    fixture.commands == std::vector<uint32_t>({7, 7, 8, 4}), "profile/status worker protocol");
        for (auto q : {R"({"dsid":"42"})", R"({"adamId":"42"})", R"({"refresh":"1"})", R"({"refresh":""})",
                       R"({"refresh":"true","account":"42"})"})
            require(runtime.send("/profile", q).status == NMW_INVALID, "unexpected profile input accepted");
        require(fixture.commands.size() == 4, "invalid profile request reached worker");
        NMWOperation op;
        uint8_t body = 1;
        auto invalid = nmw_send(runtime.value, &op, "/profile", "{}", &body, 1, "");
        require(invalid.status == NMW_INVALID, "profile body accepted");
        nmw_free_bytes(invalid.metadata); nmw_free_bytes(invalid.binary);
    });
    r.run("profile absence and malformed optional fields preserve a usable response", [] {
        Boundary fixture;
        Runtime runtime({}, {}, {}, &fixture, Boundary::invoke);
        for (bool present : {false, true}) {
            fixture.output = bytes(Json{{"profile_present", present}, {"name", std::string(1025, 'x')},
                {"handle", "bad\nhandle"}, {"image", Json::array()}, {"credentials", "private-token"}});
            auto result = runtime.send();
            require(result.status == NMW_OK && result.envelope["data"] == Json{{"profile_present", present},
                {"name", nullptr}, {"handle", nullptr}, {"image", nullptr}}, "optional profile metadata leaked");
        }
        fixture.output = bytes(Json{{"profile_present", true}});
        require(runtime.send().envelope["data"]["image"].is_null(), "missing photo rejected");
    });
    r.run("profile photos accept only bounded CDN metadata", [] {
        Boundary fixture;
        Runtime runtime({}, {}, {}, &fixture, Boundary::invoke);
        const std::vector<std::string> invalid = {"http://is1-ssl.mzstatic.com/image", "https://example.invalid/image",
            "https://is1-ssl.mzstatic.com.example.invalid/image", "https://user:secret@is1-ssl.mzstatic.com/image",
            "https://is1-ssl.mzstatic.com:444/image", "https://is1-ssl.mzstatic.com/image?token=secret",
            "https://is1-ssl.mzstatic.com/image#secret", "https://is1-ssl.mzstatic.com/{token}",
            std::string("https://is1-ssl.mzstatic.com/") + std::string(4096, 'x'),
            std::string("https://is1-ssl.mzstatic.com/a\0secret", 36)};
        for (const auto& url : invalid) {
            fixture.output = bytes(Json{{"profile_present", true}, {"image", {{"url", url}}}});
            require(runtime.send().envelope["data"]["image"].is_null(), "unsafe profile image accepted");
        }
        for (const auto& dimension : {Json(-1), Json(0), Json(16385), Json(512.5), Json(true), Json("512"),
                                      Json(nullptr), Json(UINT64_MAX)}) {
            auto photo = image;
            photo["width"] = dimension;
            fixture.output = bytes(Json{{"profile_present", true}, {"image", photo}});
            require(runtime.send().envelope["data"]["image"].is_null(), "invalid dimension accepted");
        }
        fixture.output = bytes(Json{{"profile_present", true}, {"image", {{"url", image["url"]}}}});
        require(runtime.send().envelope["data"]["image"] == Json{{"url", image["url"]}}, "optional dimensions required");
    });
    r.run("profile crop templates survive the validated worker projection", [] {
        Boundary fixture;
        fixture.output = bytes(Json{{"profile_present", true}, {"image", image}});
        Runtime runtime({}, {}, {}, &fixture, Boundary::invoke);
        for (auto query : {"{}", R"({"refresh":"true"})"}) {
            auto result = runtime.send("/profile", query);
            require(result.status == NMW_OK && result.envelope["data"]["image"] == image,
                    "valid Apple crop template was discarded or rewritten");
        }
    });
    r.run("profile failures and malformed worker replies retain transport errors", [] {
        Boundary fixture;
        Runtime runtime({}, {}, {}, &fixture, Boundary::invoke);
        for (int code : {NMW_REJECTED, NMW_UNAVAILABLE, NMW_TIMEOUT, NMW_CANCELLED}) {
            fixture.error = code;
            require(runtime.send().status == code, "profile failure hidden");
        }
        fixture.error = NMW_NO_ACCOUNT;
        require(runtime.send().envelope["code"] == 401, "signed-out profile code");
        fixture.error = NMW_OK;
        for (const auto& value : {Json::array(), Json::object(), Json{{"profile_present", "true"}}}) {
            fixture.output = bytes(value);
            require(runtime.send().status == NMW_INVALID, "malformed profile reply accepted");
        }
        fixture.output = Bytes{'{'};
        require(runtime.send().status == NMW_INVALID, "invalid JSON accepted");
        fixture.output = Bytes(native_poc::maximumProfileReply + 1, ' ');
        require(runtime.send().status == NMW_OVERSIZED, "profile reply bound");
    });
    r.run("real SDK profile endpoint matches only the active account and preserves cached metadata", [&] {
        for (const auto& shape : {"matching", "no photo", "no profile", "other account", "signed out", "invalid metadata"}) {
            Temp t;
            Runtime runtime(build / "wrapper-native-poc", assets, t.path);
            initialize(runtime);
            const std::string test = shape;
            {
                Database db(t.path);
                db.execute("INSERT INTO account(active,dsid,id,firstName,storeFront)"
                           "VALUES(1,42,'fictional@example.invalid','Account Name','143441-1,31')");
                if (test != "no profile")
                    db.profile(test == "other account" ? 84 : 42,
                        test == "invalid metadata" ? std::string(1025, 'x') : name, "fixture.handle",
                        test == "no photo" ? "" : test == "invalid metadata" ? R"({"url":"http://example.invalid/private"})" : image.dump());
                if (test == "signed out") db.execute("UPDATE account SET active=0");
            }
            auto result = runtime.send();
            require(result.status == NMW_OK, "real SDK profile transport: " + test);
            if (test == "signed out") {
                require(result.envelope["code"] == 401 &&
                            runtime.send("/profile", R"({"refresh":"true"})").envelope["code"] == 401,
                        "signed-out profile/refresh disclosed data");
                continue;
            }
            const auto& data = result.envelope["data"];
            bool present = test != "no profile" && test != "other account";
            require(result.envelope["code"] == 0 && data["profile_present"] == present && data.size() == 4,
                    "active-account profile match: " + test);
            if (present) {
                require(data["handle"] == "fixture.handle", "SDK borrowed handle");
                require(test == "invalid metadata" ? data["name"].is_null() : data["name"] == name, "SDK Data name");
                require(test == "no photo" || test == "invalid metadata" ? data["image"].is_null() : data["image"] == image,
                        "SDK optional image: " + test);
            } else require(data["name"].is_null() && data["handle"].is_null() && data["image"].is_null(), "other account metadata");
            require(result.envelope.dump().find("private-") == std::string::npos, "SDK private fields leaked");
            for (unsigned n = 0; n < 20; ++n) require(runtime.send().envelope == result.envelope, "profile reuse changed result");
            Database db(t.path);
            sqlite3_stmt* statement = nullptr;
            require(sqlite3_prepare_v2(db.value, "SELECT bio,socialProfileId FROM profile LIMIT 1", -1, &statement, nullptr) == SQLITE_OK,
                    "read-only metadata query");
            if (sqlite3_step(statement) == SQLITE_ROW)
                require(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) == "private biography" &&
                        std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))) == "private-social-id", "profile read mutated cache");
            sqlite3_finalize(statement);
        }
    });
    r.run("profile account replacement discards the old account photo", [&] {
        Temp t;
        Runtime runtime(build / "wrapper-native-poc", assets, t.path);
        initialize(runtime);
        {
            Database db(t.path);
            db.execute("INSERT INTO account(active,dsid,id) VALUES(1,42,'first@example.invalid')");
            db.profile(42, "First", "first", image.dump());
            db.profile(84, "Second", "second", "");
        }
        require(runtime.send().envelope["data"]["image"] == image, "first account photo");
        NMWOperation account;
        require(nmw_begin_account(runtime.value, &account) == NMW_OK, "account reservation");
        require(runtime.send().status == NMW_BUSY, "profile raced account replacement");
        {
            Database db(t.path);
            db.execute("UPDATE account SET active=0; INSERT INTO account(active,dsid,id) VALUES(1,84,'second@example.invalid')");
        }
        nmw_end_account(runtime.value);
        auto data = runtime.send().envelope["data"];
        require(data["name"] == "Second" && data["handle"] == "second" && data["image"].is_null(), "old profile photo survived switch");
    });
    r.run("real profile HTTP route returns private bounded metadata without logging it", [&] {
        Temp t;
        Runtime runtime(build / "wrapper-native-poc", assets, t.path);
        initialize(runtime);
        {
            Database db(t.path);
            db.execute("INSERT INTO account(active,dsid,id) VALUES(1,42,'fictional@example.invalid')");
            db.profile(42, name, "fixture.handle", image.dump());
        }
        int port = freePort();
        Process server({(build / "native-music-wrapper-server").string(), "--binary", (build / "wrapper-native-poc").string(),
            "--assets", assets.string(), "--data-dir", t.path.string(), "--port", std::to_string(port)}, t.path);
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(1);
        client.set_read_timeout(3);
        auto deadline = Clock::now() + std::chrono::seconds(5);
        httplib::Result response;
        do {
            response = client.Get("/profile");
            if (response) break;
            usleep(10000);
        } while (Clock::now() < deadline);
        require(response && response->status == 200, "profile HTTP response");
        auto result = Json::parse(response->body);
        require(result["code"] == 0 && result["msg"] == "SUCCESS" &&
                result["data"]["profile_present"] == true && result["data"]["name"] == name &&
                result["data"]["image"] == image && response->body.size() < native_poc::maximumProfileReply,
                "profile HTTP metadata");
        require(response->get_header_value("Cache-Control") == "no-store", "profile cache header");
        auto log = read(t.path / "http-timings.jsonl");
        require(log.find("/profile") != log.npos && log.find("fixture.handle") == log.npos &&
                log.find("mzstatic") == log.npos && log.find("private-") == log.npos, "profile entered timing logs");
    });
    r.run("profile private-pipe cancellation releases queued requests and restarts cleanly", [&] {
        Temp t;
        Runtime runtime(build / "native-wrapper-tests", "profile-stall-fixture", t.path);
        NMWOperation active, queued;
        auto running = std::async(std::launch::async, [&] { return runtime.send("/profile", "{}", &active).status; });
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!fs::exists(t.path / "entered") && Clock::now() < deadline) std::this_thread::yield();
        auto waiter = std::async(std::launch::async, [&] { return runtime.send("/profile", "{}", &queued).status; });
        while (!nmw_pending_requests(runtime.value) && Clock::now() < deadline) std::this_thread::yield();
        nmw_operation_cancel(&queued);
        bool prompt = waiter.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
        nmw_operation_cancel(&active);
        require(waiter.get() == NMW_CANCELLED && running.get() == NMW_CANCELLED && prompt, "profile cancellation blocked");
        write(t.path / "resume", "");
        auto result = runtime.send();
        require(result.status == NMW_OK && result.envelope["data"]["profile_present"] == false, "cancelled worker reused stale output");
    });
    r.run("profile refresh enforces a wall deadline including stalled SDK bootstrap", [&] {
        Temp t;
        Runtime runtime(build / "native-wrapper-tests", "profile-stall-fixture", t.path);
        auto start = Clock::now();
        auto result = runtime.send("/profile", R"({"refresh":"true"})");
        auto elapsed = Clock::now() - start;
        require(result.status == NMW_TIMEOUT && elapsed >= std::chrono::seconds(native_poc::profileWorkerTimeoutSeconds - 1) &&
                    elapsed < std::chrono::seconds(native_poc::profileWorkerTimeoutSeconds + 3), "profile wall deadline");
        write(t.path / "resume", "");
        require(runtime.send().status == NMW_OK, "timeout did not reset worker");
    });
}
