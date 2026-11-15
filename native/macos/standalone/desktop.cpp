// AppKit is called through its public Objective-C runtime ABI from C++.
// The desktop is a client of the same C++ service shipped in the package.
#include "../kit/runtime.h"
#include "../third_party/http/httplib.h"
#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach-o/dyld.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char **environ;
namespace fs = std::filesystem;
using wrapper::Json;
namespace {
template <class R = id, class... A> R send(id receiver, const char *selector, A... args) {
    return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(receiver, sel_registerName(selector), args...);
}
id type(const char *name) { return reinterpret_cast<id>(objc_getClass(name)); }
id string(const std::string &s) { return send(type("NSString"), "stringWithUTF8String:", s.c_str()); }
std::string text(id s) {
    if (!s)
        return {};
    const char *p = send<const char *>(s, "UTF8String");
    return p ? p : "";
}
id create(const char *name, CGRect frame) { return send(send(type(name), "alloc"), "initWithFrame:", frame); }
void mainThread(std::function<void()> task) {
    auto *job = new std::function<void()>(std::move(task));
    dispatch_async_f(dispatch_get_main_queue(), job, [](void *p) {
        std::unique_ptr<std::function<void()>> job(static_cast<std::function<void()> *>(p));
        (*job)();
    });
}
std::string instanceID() {
    return std::to_string(getpid()) + "-" + std::to_string(arc4random()) + "-" + std::to_string(arc4random());
}
pid_t launch(const fs::path &binary, std::vector<std::string> args, const fs::path &log) {
    std::vector<char *> values;
    args.insert(args.begin(), binary.string());
    for (auto &s : args)
        values.push_back(s.data());
    values.push_back(nullptr);
    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO);
    pid_t pid = -1;
    int result = posix_spawn(&pid, binary.c_str(), &actions, nullptr, values.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fd);
    return result ? -1 : pid;
}
void stopOwned(pid_t &pid) {
    if (pid <= 0)
        return;
    int status = 0;
    auto r = waitpid(pid, &status, WNOHANG);
    if (r == 0) {
        kill(pid, SIGTERM);
        for (unsigned i = 0; i < 60 && r == 0; ++i) {
            usleep(10000);
            r = waitpid(pid, &status, WNOHANG);
        }
        if (r == 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    pid = -1;
}
struct Result {
    std::string name, state = "Waiting", detail;
    double ms = -1;
};
struct Desktop {
    fs::path contents, profile;
    id app{}, window{}, view{}, delegate{}, serverTab{}, checksTab{}, serverPane{}, checksPane{}, status{},
        account{}, notice{}, portField{}, startButton{}, stopButton{}, restartButton{}, signButton{}, copyButton{},
        trackField{}, checkButton{}, table{}, summaryButton{}, lastChecked{};
    pid_t serverPID = -1, loginPID = -1;
    int port = 12341;
    bool running = false, busy = false, checking = false, privateCheck = false;
    unsigned generation = 0;
    std::string instance, address, summary;
    std::vector<Result> rows;
    void resetRows() {
        rows.clear();
        for (auto s : {"Account status", "Device asset", "Native key", "Syllable lyrics", "Line lyrics",
                       "Web playback", "License"})
            rows.push_back({s, "Waiting", "", -1});
    }
    id label(id parent, const std::string &value, CGRect frame, bool title = false) {
        id v = send(type("NSTextField"), "labelWithString:", string(value));
        send<void>(v, "setFrame:", frame);
        send<void>(v, "setSelectable:", true);
        if (title)
            send<void>(v, "setFont:", send(type("NSFont"), "boldSystemFontOfSize:", CGFloat(23)));
        send<void>(parent, "addSubview:", v);
        return v;
    }
    id button(id parent, const char *name, const char *action, CGRect frame) {
        id b = send(type("NSButton"), "buttonWithTitle:target:action:", string(name), delegate,
                    sel_registerName(action));
        send<void>(b, "setFrame:", frame);
        send<void>(parent, "addSubview:", b);
        return b;
    }
    id field(id parent, const std::string &value, CGRect frame) {
        auto f = create("NSTextField", frame);
        send<void>(f, "setStringValue:", string(value));
        send<void>(parent, "addSubview:", f);
        return f;
    }
    void set(id target, const std::string &value) { send<void>(target, "setStringValue:", string(value)); }
    void refresh() {
        send<void>(portField, "setEnabled:", !running && !busy);
        send<void>(startButton, "setEnabled:", !running && !busy);
        send<void>(stopButton, "setEnabled:", running && !busy);
        send<void>(signButton, "setEnabled:", !busy && !checking);
        send<void>(restartButton, "setEnabled:", running && !busy);
        send<void>(copyButton, "setEnabled:", running);
        send<void>(checkButton, "setEnabled:", running && !busy && !checking);
        send<void>(trackField, "setEnabled:", !checking);
        send<void>(summaryButton, "setEnabled:", !summary.empty() && !checking);
    }
    void select(bool checks) {
        send<void>(checksPane, "setHidden:", !checks);
        send<void>(serverPane, "setHidden:", checks);
        send<void>(serverTab, "setState:", long(!checks));
        send<void>(checksTab, "setState:", long(checks));
        send<void>(window, "setTitle:", string(checks ? "Endpoint Checks" : "Native Music Wrapper"));
    }
    void setup() {
        resetRows();
        window = send(send(type("NSWindow"), "alloc"),
                      "initWithContentRect:styleMask:backing:defer:", CGRectMake(0, 0, 850, 542),
                      static_cast<unsigned long>(15), static_cast<unsigned long>(2), false);
        send<void>(window, "setMinSize:", CGSizeMake(820, 560));
        send<void>(window, "setDelegate:", delegate);
        send<void>(window, "setReleasedWhenClosed:", false);
        view = send(window, "contentView");
        serverTab = button(view, "Server", "showServer:", CGRectMake(16, 472, 150, 32));
        checksTab = button(view, "Endpoint Checks", "showChecks:", CGRectMake(16, 432, 150, 32));
        for (auto b : {serverTab, checksTab}) {
            send<void>(b, "setButtonType:", static_cast<unsigned long>(2));
            send<void>(b, "setAutoresizingMask:", static_cast<unsigned long>(8));
        }
        serverPane = create("NSView", CGRectMake(180, 16, 650, 510));
        checksPane = create("NSView", CGRectMake(180, 16, 650, 510));
        for (auto pane : {serverPane, checksPane}) {
            send<void>(view, "addSubview:", pane);
            send<void>(pane, "setAutoresizingMask:", static_cast<unsigned long>(2 | 16));
        }
        label(serverPane, "Server", CGRectMake(18, 444, 570, 40), true);
        status = label(serverPane, "Starting…", CGRectMake(18, 400, 565, 26));
        account = label(serverPane, "Checking saved account…", CGRectMake(18, 364, 565, 26));
        label(serverPane, "Port", CGRectMake(18, 308, 56, 24));
        portField = field(serverPane, std::to_string(port), CGRectMake(76, 306, 100, 26));
        startButton = button(serverPane, "Start Server", "startServer:", CGRectMake(18, 255, 122, 32));
        stopButton = button(serverPane, "Stop Server", "stopServer:", CGRectMake(146, 255, 120, 32));
        restartButton = button(serverPane, "Restart", "restartServer:", CGRectMake(274, 255, 92, 32));
        copyButton = button(serverPane, "Copy URL", "copyURL:", CGRectMake(374, 255, 105, 32));
        signButton = button(serverPane, "Sign In…", "signIn:", CGRectMake(18, 198, 115, 32));
        button(serverPane, "Show Timings", "showTimings:", CGRectMake(142, 198, 138, 32));
        notice = label(serverPane, "", CGRectMake(18, 113, 600, 66));
        send<void>(notice, "setLineBreakMode:", static_cast<long>(0));
        label(checksPane, "Endpoint Checks", CGRectMake(18, 444, 570, 40), true);
        label(checksPane, "Catalog track ID", CGRectMake(18, 399, 113, 24));
        trackField = field(checksPane, "1552791761", CGRectMake(141, 397, 215, 26));
        send<void>(trackField, "setTarget:", delegate);
        send<void>(trackField, "setAction:", sel_registerName("runChecks:"));
        checkButton = button(checksPane, "Run Checks", "runChecks:", CGRectMake(368, 394, 126, 32));
        auto scroll = create("NSScrollView", CGRectMake(18, 88, 607, 280));
        send<void>(scroll, "setHasVerticalScroller:", true);
        send<void>(scroll, "setBorderType:", static_cast<unsigned long>(2));
        send<void>(scroll, "setAutoresizingMask:", static_cast<unsigned long>(2 | 16));
        table = create("NSTableView", CGRectMake(0, 0, 600, 280));
        for (auto spec : {std::pair{"Endpoint", 145.0}, std::pair{"Result", 75.0}, std::pair{"Time", 76.0},
                          std::pair{"Details", 288.0}}) {
            auto col = send(send(type("NSTableColumn"), "alloc"), "initWithIdentifier:", string(spec.first));
            send<void>(col, "setEditable:", false);
            send<void>(col, "setTitle:", string(spec.first));
            send<void>(col, "setWidth:", CGFloat(spec.second));
            send<void>(table, "addTableColumn:", col);
            send<void>(col, "release");
        }
        send<void>(table, "setDataSource:", delegate);
        send<void>(table, "setDelegate:", delegate);
        send<void>(scroll, "setDocumentView:", table);
        send<void>(checksPane, "addSubview:", scroll);
        lastChecked = label(checksPane, "", CGRectMake(18, 35, 255, 28));
        button(checksPane, "Show Timings", "showTimings:", CGRectMake(320, 31, 133, 32));
        summaryButton = button(checksPane, "Copy Summary", "copySummary:", CGRectMake(457, 31, 150, 32));
        select(false);
        refresh();
        send<void>(window, "center");
        send<void>(window, "makeKeyAndOrderFront:", id(nil));
        send(type("NSTimer"), "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", 1.0, delegate,
             sel_registerName("checkProcess:"), id(nil), true);
        send<void>(app, "activateIgnoringOtherApps:", true);
        start();
    }
    std::string cell(long row, const std::string &column) {
        if (row < 0 || static_cast<size_t>(row) >= rows.size())
            return {};
        const auto &r = rows[row];
        if (column == "Endpoint")
            return r.name;
        if (column == "Result")
            return r.state;
        if (column == "Time")
            return r.ms < 0 ? "—" : std::to_string(static_cast<unsigned>(r.ms)) + " ms";
        return r.detail;
    }
    static Json get(int port, const std::string &route, httplib::Params params = {}, int seconds = 80) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(1);
        client.set_read_timeout(seconds);
        auto path = httplib::append_query_params(route, params);
        auto r = client.Get(path);
        if (!r || r->status != 200 || r->get_header_value("X-Wrapper-Backend") != "native-macos-arm64")
            throw std::runtime_error("No native server response");
        auto j = Json::parse(r->body);
        if (j.value("code", -1) != 0)
            throw std::runtime_error("Wrapper request failed");
        return j.at("data");
    }
    void start() {
        if (busy || running)
            return;
        try {
            auto input = text(send(portField, "stringValue"));
            if (!std::regex_match(input, std::regex("[0-9]{1,5}")))
                throw 0;
            port = std::stoi(input);
            if (port <= 0 || port > 65535)
                throw 0;
        } catch (...) {
            set(notice, "Enter a port between 1 and 65535.");
            return;
        }
        std::error_code e;
        fs::create_directories(profile, e);
        if (e) {
            set(notice, "Could not open the account folder.");
            return;
        }
        chmod(profile.c_str(), 0700);
        instance = instanceID();
        address = "http://127.0.0.1:" + std::to_string(port);
        auto bin = contents / "MacOS" / "wrapper-native-poc";
        auto assets = contents / "Resources" / "Runtime" / "assets";
        serverPID = launch(contents / "MacOS" / "native-music-wrapper-server",
                           {"--binary", bin.string(), "--assets", assets.string(), "--data-dir", profile.string(),
                            "--port", std::to_string(port), "--instance-id", instance, "--owner-pid",
                            std::to_string(getpid())},
                           profile / "native-http.log");
        if (serverPID < 0) {
            set(notice, "Could not launch the native server.");
            return;
        }
        auto gen = ++generation;
        busy = true;
        refresh();
        set(status, "Starting…");
        set(notice, "");
        std::thread([this, gen, p = port, expected = instance] {
            Json result;
            for (unsigned i = 0; i < 50; ++i) {
                try {
                    auto data = get(p, "/status", {}, 3);
                    if (data.value("server_instance", "") == expected) {
                        result = std::move(data);
                        break;
                    }
                } catch (...) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
            }
            mainThread([this, gen, result = std::move(result)] {
                if (gen != generation)
                    return;
                busy = false;
                running = result.is_object();
                if (running) {
                    set(status, "Running at " + address);
                    std::string region;
                    if (result.contains("regions") && result["regions"].is_array() && !result["regions"].empty() &&
                        result["regions"][0].is_string())
                        region = " · " + result["regions"][0].get<std::string>();
                    set(account, result.value("account_present", false) ? "Apple Account signed in" + region
                                                                        : "Apple Account signed out");
                    if (!privateCheck)
                        send<void>(send(type("NSUserDefaults"), "standardUserDefaults"),
                                   "setInteger:forKey:", long(port), string("nativeServerPort"));
                } else {
                    stopOwned(serverPID);
                    set(status, "Stopped");
                    set(notice, "The server did not become ready. Check the port and try again.");
                }
                refresh();
            });
        }).detach();
    }
    void stop() {
        ++generation;
        stopOwned(serverPID);
        running = busy = checking = false;
        set(status, "Stopped");
        set(account, "");
        refresh();
    }
    void pollServer() {
        if (running && serverPID > 0) {
            int statusCode = 0;
            if (waitpid(serverPID, &statusCode, WNOHANG) == serverPID) {
                serverPID = -1;
                stop();
                set(notice, "The native server stopped.");
            }
        }
    }
    void signIn() {
        if (busy || checking)
            return;
        stop();
        busy = true;
        refresh();
        set(status, "Signing in…");
        auto bin = contents / "MacOS" / "wrapper-native-poc";
        auto assets = contents / "Resources" / "Runtime" / "assets";
        auto pid = launch(bin,
                          {"--assets", assets.string(), "--apple-profile", "--probe", "auth-login", "--data-dir",
                           profile.string(), "--prompt-script", "@system-dialog", "--owner-pid",
                           std::to_string(getpid())},
                          "/dev/null");
        if (pid < 0) {
            busy = false;
            refresh();
            set(notice, "Could not start sign-in.");
            return;
        }
        loginPID = pid;
        auto gen = generation;
        std::thread([this, pid, gen] {
            int s = 0;
            while (waitpid(pid, &s, 0) < 0 && errno == EINTR) {
            }
            mainThread([this, gen, s] {
                if (gen != generation)
                    return;
                loginPID = -1;
                busy = false;
                start();
                if (!WIFEXITED(s) || WEXITSTATUS(s))
                    set(notice, "Sign-in did not complete.");
            });
        }).detach();
    }
    void check() {
        if (!running || busy || checking)
            return;
        auto id = text(send(trackField, "stringValue"));
        if (!wrapper::validID(id)) {
            set(lastChecked, "Enter a numeric song ID.");
            return;
        }
        resetRows();
        checking = true;
        refresh();
        auto gen = generation;
        send<void>(table, "reloadData");
        std::thread([this, p = port, id, gen] {
            std::string key;
            std::vector<Result> results;
            const std::vector<std::pair<std::string, std::string>> steps = {
                {"Account status", "/status"},  {"Device asset", "/m3u8"},  {"Native key", "/key"},
                {"Syllable lyrics", "/lyrics"}, {"Line lyrics", "/lyrics"}, {"Web playback", "/webplayback"},
                {"License", "/license"}};
            for (size_t i = 0; i < steps.size(); ++i) {
                Result row{steps[i].first, "Passed", "", -1};
                auto start = wrapper::Clock::now();
                try {
                    if (i == 6) {
                        row.state = "Limited";
                        row.detail = "A DRM client challenge is required.";
                    } else if (i == 2 && key.empty()) {
                        row.state = "Limited";
                        row.detail = "No track key in this playlist.";
                    } else {
                        httplib::Params q;
                        if (i)
                            q.emplace("adamId", id);
                        if (i == 2)
                            q.emplace("uri", key);
                        if (i == 3 || i == 4)
                            q.emplace("syllable", i == 3 ? "1" : "0");
                        auto data = get(p, steps[i].second, q);
                        if (i == 0)
                            row.detail = data.value("account_present", false) ? "Signed in" : "Signed out";
                        else if (i == 1) {
                            row.detail = data.value("streamType", "hls") == "file" ? "Direct protected file"
                                                                                   : "HLS playlist";
                            auto url = wrapper::field(data, "m3u8");
                            NMWOperation op;
                            for (int depth = 0; depth < 3 && row.detail == "HLS playlist" && !url.empty();
                                 ++depth) {
                                if (wrapper::urlHost(url, true).empty())
                                    break;
                                auto raw = wrapper::https({url, {}, {}}, op);
                                std::string list(raw.begin(), raw.end());
                                std::istringstream lines(list);
                                std::string line, child;
                                while (std::getline(lines, line)) {
                                    if (!line.empty() && line.back() == '\r')
                                        line.pop_back();
                                    if (line.rfind("#EXT-X-KEY:", 0) == 0 ||
                                        line.rfind("#EXT-X-SESSION-KEY:", 0) == 0) {
                                        auto k = line.find("URI=\"skd://");
                                        if (k != std::string::npos) {
                                            auto end = line.find('"', k + 5);
                                            auto value = line.substr(k + 5, end - k - 5);
                                            if (value != "skd://itunes.apple.com/P000000000/s1/e1") {
                                                key = value;
                                                break;
                                            }
                                        }
                                    } else if (child.empty() && !line.empty() && line.front() != '#')
                                        child = line;
                                }
                                if (!key.empty() || child.empty())
                                    break;
                                url = child.rfind("https://", 0) == 0 ? child
                                                                      : url.substr(0, url.rfind('/') + 1) + child;
                            }
                        } else if (i == 2)
                            row.detail = "Native key available";
                        else if (i == 3 || i == 4)
                            row.detail = std::to_string(wrapper::field(data, "lyrics").size()) + " bytes";
                        else
                            row.detail = data.value("streamType", "hls");
                    }
                } catch (...) {
                    row.state = "Failed";
                    row.detail = "Request unavailable.";
                }
                row.ms = std::chrono::duration<double, std::milli>(wrapper::Clock::now() - start).count();
                results.push_back(row);
                mainThread([this, gen, i, row] {
                    if (generation == gen) {
                        rows[i] = row;
                        send<void>(table, "reloadData");
                    }
                });
            }
            mainThread([this, gen, results] {
                if (generation != gen)
                    return;
                checking = false;
                summary.clear();
                for (auto &row : results)
                    summary += row.name + " · " + row.state + " · " + row.detail + "\n";
                set(lastChecked, "Checks complete");
                refresh();
            });
        }).detach();
    }
    void copy(const std::string &value) {
        auto p = send(type("NSPasteboard"), "generalPasteboard");
        send<long>(p, "clearContents");
        send<bool>(p, "setString:forType:", string(value), string("public.utf8-plain-text"));
    }
    void showTimings() {
        auto path = profile / "http-timings.jsonl";
        send<bool>(send(type("NSWorkspace"), "sharedWorkspace"),
                   "selectFile:inFileViewerRootedAtPath:", string(path.string()), string(""));
    }
    void shutdown() {
        ++generation;
        stopOwned(serverPID);
        stopOwned(loginPID);
    }
};
Desktop *desktop;
void launched(id, SEL, id) { desktop->setup(); }
void ending(id, SEL, id) { desktop->shutdown(); }
bool lastWindow(id, SEL, id) { return true; }
long rowCount(id, SEL, id) { return static_cast<long>(desktop->rows.size()); }
id columnValue(id, SEL, id, id column, long row) {
    return string(desktop->cell(row, text(send(column, "identifier"))));
}
void action(id, SEL selector, id) {
    std::string s = sel_getName(selector);
    if (s == "showServer:")
        desktop->select(false);
    else if (s == "showChecks:")
        desktop->select(true);
    else if (s == "startServer:")
        desktop->start();
    else if (s == "stopServer:")
        desktop->stop();
    else if (s == "restartServer:") {
        desktop->stop();
        desktop->start();
    } else if (s == "checkProcess:")
        desktop->pollServer();
    else if (s == "signIn:")
        desktop->signIn();
    else if (s == "runChecks:")
        desktop->check();
    else if (s == "copyURL:")
        desktop->copy(desktop->address);
    else if (s == "copySummary:")
        desktop->copy(desktop->summary);
    else if (s == "showTimings:")
        desktop->showTimings();
}
} // namespace
int main(int argc, char **argv) {
    umask(0077);
    auto pool = send(send(type("NSAutoreleasePool"), "alloc"), "init");
    Desktop state;
    desktop = &state;
    state.app = send(type("NSApplication"), "sharedApplication");
    send<bool>(state.app, "setActivationPolicy:", long(0));
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    _NSGetExecutablePath(path.data(), &size);
    state.contents = fs::canonical(path.data()).parent_path().parent_path();
    auto defaults = send(type("NSUserDefaults"), "standardUserDefaults");
    auto saved = text(send(defaults, "stringForKey:", string("nativeProfilePath")));
    state.profile = saved.empty()
                        ? fs::path(std::getenv("HOME")) / "Library/Application Support/Native Music Wrapper"
                        : fs::path(saved);
    auto p = send<long>(defaults, "integerForKey:", string("nativeServerPort"));
    if (p > 0 && p <= 65535)
        state.port = static_cast<int>(p);
    for (int i = 1; i + 1 < argc; ++i) {
        std::string key = argv[i];
        if (key == "--profile")
            state.profile = argv[++i];
        else if (key == "--port") {
            state.port = std::stoi(argv[++i]);
            state.privateCheck = true;
        }
    }
    if (!state.privateCheck && !state.profile.empty())
        send<void>(defaults, "setObject:forKey:", string(state.profile.string()), string("nativeProfilePath"));
    Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "NativeWrapperDesktopDelegate", 0);
    class_addMethod(cls, sel_registerName("applicationDidFinishLaunching:"), reinterpret_cast<IMP>(launched),
                    "v@:@");
    class_addMethod(cls, sel_registerName("applicationWillTerminate:"), reinterpret_cast<IMP>(ending), "v@:@");
    class_addMethod(cls, sel_registerName("applicationShouldTerminateAfterLastWindowClosed:"),
                    reinterpret_cast<IMP>(lastWindow), "B@:@");
    class_addMethod(cls, sel_registerName("numberOfRowsInTableView:"), reinterpret_cast<IMP>(rowCount), "q@:@");
    class_addMethod(cls, sel_registerName("tableView:objectValueForTableColumn:row:"),
                    reinterpret_cast<IMP>(columnValue), "@@:@@q");
    for (auto s : {"showServer:", "showChecks:", "startServer:", "stopServer:", "restartServer:", "checkProcess:",
                   "signIn:", "runChecks:", "copyURL:", "copySummary:", "showTimings:"})
        class_addMethod(cls, sel_registerName(s), reinterpret_cast<IMP>(action), "v@:@");
    objc_registerClassPair(cls);
    state.delegate = send(send(reinterpret_cast<id>(cls), "alloc"), "init");
    send<void>(state.app, "setDelegate:", state.delegate);
    auto menu = send(send(type("NSMenu"), "alloc"), "initWithTitle:", string(""));
    auto mainItem = send(send(type("NSMenuItem"), "alloc"), "init");
    auto sub = send(send(type("NSMenu"), "alloc"), "initWithTitle:", string("Native Music Wrapper"));
    send(sub, "addItemWithTitle:action:keyEquivalent:", string("Quit Native Music Wrapper"),
         sel_registerName("terminate:"), string("q"));
    send<void>(mainItem, "setSubmenu:", sub);
    send<void>(menu, "addItem:", mainItem);
    auto editItem = send(send(type("NSMenuItem"), "alloc"), "init");
    auto editMenu = send(send(type("NSMenu"), "alloc"), "initWithTitle:", string("Edit"));
    const char *editActions[][3] = {
        {"Cut", "cut:", "x"}, {"Copy", "copy:", "c"}, {"Paste", "paste:", "v"}, {"Select All", "selectAll:", "a"}};
    for (auto &entry : editActions)
        send(editMenu, "addItemWithTitle:action:keyEquivalent:", string(entry[0]), sel_registerName(entry[1]),
             string(entry[2]));
    send<void>(editItem, "setSubmenu:", editMenu);
    send<void>(menu, "addItem:", editItem);
    send<void>(state.app, "setMainMenu:", menu);
    send<void>(state.app, "run");
    send<void>(pool, "drain");
    return 0;
}
