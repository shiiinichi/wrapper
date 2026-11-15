#include "test_support.h"
#include "../music_client.h"
#include <sqlite3.h>
#include <sys/stat.h>
using namespace validation;
namespace {
Json invoke(const fs::path &worker, const fs::path &assets, std::string mode, std::string name,
            const fs::path &profile = {}, bool apple = true) {
    Temp output;
    std::vector<std::string> args = {worker.string(), "--assets", assets.string(), mode, name};
    if (apple)
        args.push_back("--apple-profile");
    if (!profile.empty()) {
        args.push_back("--data-dir");
        args.push_back(profile.string());
    }
    Process child(args, output.path);
    auto rc = child.wait();
    auto report = Json::parse(read(child.out));
    require(rc == 0 && report.value("ok", false), "native probe " + name + ": " + report.value("error", "failed"));
    return report;
}
bool includes(const Json &list, const std::string &text) {
    return std::find(list.begin(), list.end(), text) != list.end();
}
void allTrue(const Json &flags) {
    for (const auto &f : flags.items())
        if (f.value().is_boolean())
            require(f.value().get<bool>() == (f.key() != "credentials_submitted"), "ABI flag failed: " + f.key());
}
std::string hex(const std::string &data) {
    std::string s;
    for (unsigned char c : data) {
        s += "0123456789abcdef"[c >> 4];
        s += "0123456789abcdef"[c & 15];
    }
    return s;
}
std::string decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i += 2)
        out.push_back(static_cast<char>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}
void databaseCheck(const fs::path &p) {
    sqlite3 *db = nullptr;
    require(sqlite3_open_v2(p.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "missing saved database");
    sqlite3_stmt *stmt = nullptr;
    require(sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, nullptr) == SQLITE_OK, "integrity query");
    require(sqlite3_step(stmt) == SQLITE_ROW &&
                std::string(reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0))) == "ok",
            "database integrity");
    sqlite3_finalize(stmt);
    require(sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' LIMIT 1", -1, &stmt,
                               nullptr) == SQLITE_OK &&
                sqlite3_step(stmt) == SQLITE_ROW,
            "missing schema");
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}
} // namespace
void loaderTests(Report &r, const fs::path &worker, const fs::path &assets, const fs::path &fixtures) {
    for (auto probe : {"coreadi-jni", "lskd-jni"})
        r.run(std::string("real Android JNI ") + probe, [&] {
            auto j = invoke(worker, assets, "--probe", probe);
            require(j["result"]["jni_version"] == 0x10006 && j["result"]["get_env_calls"] == 0 &&
                        j["loader"]["relocated_without_blockers"] == true,
                    "JNI contract");
        });
    for (auto lib : {"libstoreservicescore.so", "libandroidappmusic.so"})
        r.run(std::string("constructor chain ") + lib, [&] {
            auto j = invoke(worker, assets, "--initialize", lib);
            size_t total = 0;
            std::map<std::string, int> counts;
            for (auto &img : j["loader"]["images"]) {
                auto name = img.at("library").get<std::string>();
                counts[name] = img["constructors_called"];
                total += counts[name];
                require(img["missing_symbols"].empty(), "missing native symbols");
                for (auto s :
                     {"CFStringCreateWithFormat", "CFStringAppendFormat", "curl_easy_setopt", "curl_share_setopt"})
                    require(!includes(img["guarded_symbols"], s), "guarded live dependency");
            }
            require(counts["libstoreservicescore.so"] == 64 && counts["libmediaplatform.so"] == 7,
                    "StoreServices construction");
            if (std::string(lib) == "libandroidappmusic.so")
                require(total == 372 && counts["libandroidappmusic.so"] == 91 &&
                            counts["libmedialibrarycore.so"] == 209,
                        "Music construction");
            require(j["loader"]["constructors_initialized"] == true, "initialization not completed");
        });
    for (auto name : {"tls-abi", "corefoundation-abi", "runtime-abi", "service-abi"})
        r.run(std::string("ARM64 ") + name, [&] {
            Temp tmp;
            fs::copy_file(fixtures / "tls-fixture.elf", tmp.path / "tls-fixture.so");
            if (std::string(name) == "runtime-abi")
                write(tmp.path / "abi-read.txt", "typed-android-file-bridge\n");
            auto result = invoke(worker, tmp.path, "--probe", name)["result"];
            allTrue(result);
            if (std::string(name) == "tls-abi")
                require(result["independent_threads"] == 3 && result["state_preserved"] == true, "TLS isolation");
            if (std::string(name) == "service-abi")
                require(read(tmp.path / "writes/abi-file") == "native-bootstrap-file" &&
                            !fs::exists(tmp.path / "outside-write") && !fs::exists(tmp.path / "outside/forbidden"),
                        "file write scope");
        });
    r.run("single Android SDK identity across fresh, flat and imported databases", [&] {
        for (auto shape : {"fresh", "flat", "imported"}) {
            Temp t;
            auto p = t.path / "account data";
            fs::create_directory(p);
            if (std::string(shape) == "flat")
                write(p / "accounts.sqlitedb", "");
            if (std::string(shape) == "imported")
                write(p / "music-client-v1", "");
            for (int n = 0; n < 2; ++n) {
                auto j = invoke(worker, assets, "--probe", "services", p);
                auto v = j["result"];
                require(v["user_agent"] == native_poc::music_client::userAgent(), "SDK used another identity");
                require(v["lease_requested"] == false && v["authenticated"] == false &&
                            v["request_context_initialized"] == true,
                        "bootstrap state");
                require(j["loader"]["relocated_without_blockers"] == true &&
                            j["loader"]["native_sqlite_opened"].get<int>() >= 4,
                        "SQLite backend");
                for (const auto &img : j["loader"]["images"])
                    require(img["mixed_write_execute_pages"] == 0, "W+X pages");
            }
            fs::path db = std::string(shape) == "flat" ? p : p / "mpl_db";
            for (auto f : {"accounts", "cookies", "httpcache", "kvs"})
                databaseCheck(db / (std::string(f) + ".sqlitedb"));
        }
    });
    r.run("blank profile cancellation and stable private device ID", [&] {
        Temp t;
        auto j = invoke(worker, assets, "--probe", "auth-preflight", t.path)["result"];
        require(j["credential_requests"] == 1 && j["cancelled"] == true && j["authenticated"] == false,
                "unexpected authentication");
        auto id = read(t.path / "native-device-id");
        require(id.size() == 16, "invalid device identity");
        auto abi = invoke(worker, assets, "--probe", "auth-abi", t.path)["result"];
        allTrue(abi);
        require(read(t.path / "native-device-id") == id, "device identity changed");
        for (auto &file : fs::recursive_directory_iterator(t.path))
            if (file.is_regular_file()) {
                struct stat st{};
                require(!stat(file.path().c_str(), &st) && (st.st_mode & 077) == 0, "account file not private");
            }
    });
    r.run("account metadata getter ABI", [&] {
        Temp t;
        allTrue(invoke(worker, assets, "--probe", "auth-account-abi", t.path)["result"]);
    });
    r.run("services require an explicit account directory", [&] {
        Temp t;
        Process p({worker.string(), "--assets", assets.string(), "--apple-profile", "--probe", "services"},
                  t.path);
        require(p.wait() == 1 && read(p.out).find("requires --data-dir") != std::string::npos,
                "missing profile accepted");
    });
    r.run("guest POSIX and stdio creation stay inside the profile", [&] {
        Temp t;
        allTrue(invoke(worker, assets, "--probe", "security-files", t.path)["result"]);
    });
    r.run("native libcurl transfers exact callback bytes", [&] {
        Temp t;
        write(t.path / "transfer.txt", "native-curl-typed-transfer\n");
        auto j = invoke(worker, t.path, "--probe", "network-abi")["result"];
        require(j["curl_typed_options"] == true, "curl ABI");
    });
    r.run("system compression and randomized stack protection", [&] {
        auto z = invoke(worker, assets, "--probe", "zlib", {}, false);
        require(z["result"]["crc32"] == 0x414FA339 && z["result"]["round_trip"] == true &&
                    z["loader"]["images"].empty(),
                "zlib roundtrip");
        auto a = invoke(worker, assets, "--probe", "security-abi", {}, false)["result"],
             b = invoke(worker, assets, "--probe", "security-abi", {}, false)["result"];
        allTrue(a);
        allTrue(b);
        require(a["stack_canary_fingerprint"] != b["stack_canary_fingerprint"], "fixed stack canary");
    });
    r.run("native blocks retain callback ownership", [&] {
        auto j = invoke(worker, assets, "--probe", "blocks", {}, false)["result"];
        require(j["invoke_result"] == 42 && j["copy_callbacks"] == 1 && j["dispose_callbacks"] == 1 &&
                    j["retain_release"] == true,
                "Blocks ABI");
    });
    r.run("unprepared mixed pages cannot execute", [&] {
        Temp t;
        auto j = invoke(worker, assets, "--inspect", "libCoreADI.so", {}, false)["loader"];
        require(j["execution_prepared"] == false && j["constructors_initialized"] == false &&
                    j["relocated_without_blockers"] == false,
                "mixed page approval");
        Process p({worker.string(), "--assets", assets.string(), "--initialize", "libCoreADI.so"}, t.path);
        require(p.wait() == 1 && Json::parse(read(p.out)).value("error", "").find("blockers") != std::string::npos,
                "unsafe initialization");
    });
    auto recipes = Json::parse(read(fixtures / "elf-cases.json"));
    auto original = read(assets / "libBlocksRuntime.so");
    for (const auto &item : recipes)
        r.run(item.at("case"), [&] {
            Temp t;
            auto data = original;
            for (const auto &patch : item.at("patches")) {
                size_t at = patch.at("offset");
                auto previous = decode(patch.at("from")), next = decode(patch.at("to"));
                require(hex(data.substr(at, previous.size())) == patch.at("from"), "pinned fixture mismatch");
                data.replace(at, previous.size(), next);
            }
            if (item.contains("size"))
                data.resize(item.at("size"));
            write(t.path / "libBlocksRuntime.so", data);
            std::string mode = item.at("mode");
            std::vector<std::string> args = {worker.string(), "--assets", t.path.string(), mode,
                                             mode == "--probe" ? "blocks" : "libBlocksRuntime.so"};
            if (item.value("apple", false))
                args.push_back("--apple-profile");
            Process p(args, t.path);
            auto rc = p.wait();
            const auto &exp = item.at("expect");
            if (exp.contains("exit"))
                require(exp.at("exit") == rc, "unexpected child exit");
            if (exp.contains("stderr")) {
                require(read(p.err).find(exp.at("stderr").get<std::string>()) != std::string::npos,
                        "guard failure missing");
                return;
            }
            auto result = Json::parse(read(p.out));
            if (exp.contains("error"))
                require(result.value("error", "").find(exp.at("error").get<std::string>()) != std::string::npos,
                        "wrong validation error");
            if (exp.contains("blocked"))
                require(result["loader"]["relocated_without_blockers"] == false, "unexpected relocation success");
            if (exp.contains("issue"))
                require(result["loader"]["issues"].dump().find(exp.at("issue").get<std::string>()) !=
                            std::string::npos,
                        "missing relocation issue");
            if (exp.contains("missing"))
                require(includes(result["loader"]["images"][0]["missing_symbols"], exp["missing"]),
                        "unreported import");
            if (exp.contains("weak"))
                require(!includes(result["loader"]["images"][0]["missing_symbols"], "nohost") &&
                            result["loader"]["images"][0]["weak_unresolved"].get<int>() > 0,
                        "invalid weak import");
        });
}
