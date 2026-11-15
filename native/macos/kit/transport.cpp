#include "runtime.h"
#include <curl/curl.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

namespace wrapper {
bool mayRetryTLSWithSystemDefault(int result, size_t bytes) {
    // Apple's system libcurl can lack an explicit TLS 1.3 mode. Both cases
    // happen before the HTTP request is transmitted. Keep certificate checks on.
    return bytes == 0 && (result == CURLE_SSL_CONNECT_ERROR || result == CURLE_NOT_BUILT_IN);
}
bool appendBounded(Bytes &destination, const char *data, size_t length, size_t limit) {
    if (destination.size() > limit || length > limit - destination.size())
        return false;
    if (length)
        destination.insert(destination.end(), data, data + length);
    return true;
}
namespace {
struct URLParts {
    CURLU *handle = curl_url();
    ~URLParts() {
        if (handle)
            curl_url_cleanup(handle);
    }
    bool set(const std::string &value) {
        return handle && !value.empty() && value.find('\0') == std::string::npos &&
               value.find_first_of("\r\n") == std::string::npos &&
               curl_url_set(handle, CURLUPART_URL, value.c_str(), 0) == CURLUE_OK;
    }
    std::string get(CURLUPart part) const {
        char *result = nullptr;
        if (curl_url_get(handle, part, &result, 0) != CURLUE_OK || !result)
            return {};
        std::string value(result);
        curl_free(result);
        return value;
    }
    bool has(CURLUPart part) const {
        char *result = nullptr;
        auto code = curl_url_get(handle, part, &result, 0);
        if (result)
            curl_free(result);
        return code == CURLUE_OK;
    }
};
void ready(int fd, short events, const NMWOperation &operation, Clock::time_point deadline) {
    for (;;) {
        check(operation);
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0)
            fail(NMW_TIMEOUT);
        pollfd p{fd, events, 0};
        auto rc = poll(&p, 1, static_cast<int>(std::min<int64_t>(remaining, 50)));
        if (rc == 0 || (rc < 0 && errno == EINTR))
            continue;
        if (rc < 0 || !(p.revents & events))
            fail(NMW_UNAVAILABLE);
        return;
    }
}
void readAll(int fd, uint8_t *dest, size_t count, const NMWOperation &op, Clock::time_point deadline) {
    while (count) {
        ready(fd, POLLIN, op, deadline);
        auto n = ::read(fd, dest, std::min<size_t>(count, 65536));
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            fail(NMW_UNAVAILABLE);
        dest += n;
        count -= static_cast<size_t>(n);
    }
}
void writeAll(int fd, const uint8_t *src, size_t count, const NMWOperation &op, Clock::time_point deadline) {
    while (count) {
        ready(fd, POLLOUT, op, deadline);
        auto n = ::write(fd, src, std::min<size_t>(count, 65536));
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            fail(NMW_UNAVAILABLE);
        src += n;
        count -= static_cast<size_t>(n);
    }
}
struct Transfer {
    Bytes data;
    const NMWOperation &op;
    bool oversized = false;
};
size_t receive(char *data, size_t size, size_t count, void *arg) {
    auto &t = *static_cast<Transfer *>(arg);
    if (t.op.cancelled || (size && count > SIZE_MAX / size))
        return 0;
    size_t n = size * count;
    try {
        if (!appendBounded(t.data, data, n, maxReply)) {
            t.oversized = true;
            return 0;
        }
    } catch (...) {
        return 0;
    }
    return n;
}
int progress(void *arg, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Transfer *>(arg)->op.cancelled ? 1 : 0;
}
struct Curl {
    CURL *handle = curl_easy_init();
    curl_slist *headers = nullptr;
    ~Curl() {
        if (headers)
            curl_slist_free_all(headers);
        if (handle)
            curl_easy_cleanup(handle);
    }
};
} // namespace
std::string urlHost(const std::string &value, bool appleOnly) {
    URLParts p;
    if (!p.set(value) || p.get(CURLUPART_SCHEME) != "https" || p.has(CURLUPART_USER) || p.has(CURLUPART_PASSWORD))
        return {};
    auto host = p.get(CURLUPART_HOST), port = p.get(CURLUPART_PORT);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto suffix = [&](const std::string &s) {
        return host.size() > s.size() && host.compare(host.size() - s.size(), s.size(), s) == 0;
    };
    if (appleOnly && ((!port.empty() && port != "443") || !(suffix(".apple.com") || suffix(".mzstatic.com"))))
        return {};
    return host;
}
bool sameOrigin(const std::string &source, const std::string &next) {
    auto host = urlHost(source);
    if (host.empty() || urlHost(next) != host)
        return false;
    URLParts a, b;
    if (!a.set(source) || !b.set(next))
        return false;
    auto port = [](const URLParts &p) {
        auto result = p.get(CURLUPART_PORT);
        return result.empty() ? std::string("443") : result;
    };
    return port(a) == port(b);
}
Bytes https(const HTTPRequest &request, const NMWOperation &op) {
    check(op);
    if (urlHost(request.url).empty())
        fail(NMW_INVALID);
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    std::string url = request.url;
    for (unsigned redirects = 0; redirects <= 5; ++redirects) {
        Curl c;
        if (!c.handle)
            fail(NMW_UNAVAILABLE);
        Transfer t{{}, op};
        auto h = c.handle;
        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_PROXY, "");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (ms <= 0)
            fail(NMW_TIMEOUT);
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(ms));
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, &t);
        for (const auto &item : request.headers) {
            auto line = item.first + ": " + item.second;
            if (line.find_first_of("\r\n") != std::string::npos)
                fail(NMW_INVALID);
            auto list = curl_slist_append(c.headers, line.c_str());
            if (!list)
                fail(NMW_UNAVAILABLE);
            c.headers = list;
        }
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, c.headers);
        if (!request.body.empty()) {
            curl_easy_setopt(h, CURLOPT_POST, 1L);
            curl_easy_setopt(h, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        }
        curl_easy_setopt(h, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_3));
        auto result = curl_easy_perform(h);
        if (mayRetryTLSWithSystemDefault(result, t.data.size()) && !op.cancelled) {
            auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0)
                fail(NMW_TIMEOUT);
            curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(remaining));
            curl_easy_setopt(h, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_DEFAULT));
            result = curl_easy_perform(h);
        }
        check(op);
        if (t.oversized)
            fail(NMW_OVERSIZED);
        if (result == CURLE_OPERATION_TIMEDOUT)
            fail(NMW_TIMEOUT);
        if (result != CURLE_OK)
            fail(NMW_UNAVAILABLE, "Apple network request failed.");
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        if (status >= 300 && status < 400) {
            char *raw = nullptr;
            curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &raw);
            if (!raw || !sameOrigin(url, raw))
                fail(NMW_INVALID, "Apple redirected outside the trusted HTTPS origin.");
            url = raw;
            continue;
        }
        if (status < 200 || status >= 300)
            fail(NMW_HTTP_BASE + static_cast<int>(status));
        return std::move(t.data);
    }
    fail(NMW_INVALID, "Too many Apple redirects.");
}

void pipeRead(int fd, uint8_t *dest, size_t n, const NMWOperation &op, Clock::time_point deadline) {
    readAll(fd, dest, n, op, deadline);
}
void pipeWrite(int fd, const uint8_t *src, size_t n, const NMWOperation &op, Clock::time_point deadline) {
    writeAll(fd, src, n, op, deadline);
}
size_t pipeReadSome(int fd, uint8_t *dest, size_t n, const NMWOperation &op, Clock::time_point deadline) {
    for (;;) {
        check(op);
        if (Clock::now() >= deadline)
            fail(NMW_TIMEOUT);
        pollfd p{fd, POLLIN, 0};
        auto r = poll(&p, 1, 50);
        if (!r || (r < 0 && errno == EINTR))
            continue;
        if (r < 0 || !(p.revents & (POLLIN | POLLHUP)))
            fail(NMW_UNAVAILABLE);
        auto got = ::read(fd, dest, n);
        if (got < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (got < 0)
            fail(NMW_UNAVAILABLE);
        return static_cast<size_t>(got);
    }
}
void PrivateChild::spawn(const std::vector<std::string> &args) {
    int in[2], out[2];
    if (pipe(in))
        fail(NMW_UNAVAILABLE);
    if (pipe(out)) {
        ::close(in[0]);
        ::close(in[1]);
        fail(NMW_UNAVAILABLE);
    }
    for (auto fd : {in[0], in[1], out[0], out[1]})
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (!rc) {
        rc = posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        if (!rc)
            rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        std::vector<char *> av;
        for (const auto &s : args)
            av.push_back(const_cast<char *>(s.c_str()));
        av.push_back(nullptr);
        if (!rc)
            rc = posix_spawn(&pid, args.at(0).c_str(), &actions, nullptr, av.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    ::close(in[0]);
    ::close(out[1]);
    if (rc) {
        ::close(in[1]);
        ::close(out[0]);
        pid = -1;
        fail(NMW_UNAVAILABLE);
    }
    input = in[1];
    output = out[0];
    for (auto fd : {input, output})
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(input, F_SETNOSIGPIPE, 1);
}
void PrivateChild::stop() {
    if (input >= 0)
        ::close(input);
    if (output >= 0)
        ::close(output);
    input = output = -1;
    if (pid > 0) {
        // An unreaped child retains its PID. Do not signal another process.
        int status = 0;
        pid_t r;
        do {
            r = waitpid(pid, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);
        if (r == 0) {
            kill(pid, SIGTERM);
            for (unsigned i = 0; i < 40 && r == 0; ++i) {
                usleep(5000);
                r = waitpid(pid, &status, WNOHANG);
            }
            if (r == 0) {
                kill(pid, SIGKILL);
                do {
                    r = waitpid(pid, &status, 0);
                } while (r < 0 && errno == EINTR);
            }
        }
        pid = -1;
    }
}
void Worker::close() {
    child.stop();
    if (profileLock >= 0) {
        ::close(profileLock);
        profileLock = -1;
    }
}
Bytes Worker::readFrame(const NMWOperation &op, Clock::time_point deadline) {
    Bytes header(4);
    readAll(child.output, header.data(), 4, op, deadline);
    size_t n = word(header, 0);
    if (!n || n > 16 * 1024 * 1024)
        fail(NMW_INVALID);
    Bytes result(n);
    readAll(child.output, result.data(), n, op, deadline);
    if (result.front() != 0)
        fail(NMW_REJECTED);
    result.erase(result.begin());
    return result;
}
void Worker::start(const NMWOperation &op, Clock::time_point deadline) {
    if (child.pid > 0)
        return;
    std::filesystem::create_directories(profile);
    if (chmod(profile.c_str(), 0700) != 0)
        fail(NMW_UNAVAILABLE);
    profileLock = ::open((std::filesystem::path(profile) / "native-api.lock").c_str(),
                         O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (profileLock < 0 || flock(profileLock, LOCK_EX | LOCK_NB))
        fail(NMW_BUSY);
    child.spawn({binary, "--assets", assets, "--apple-profile", "--decryption-worker", "--data-dir", profile,
                 "--owner-pid", std::to_string(getpid())});
    ++version;
    if (readFrame(op, deadline) != Bytes{'N', 'A', 'D', '2'})
        fail(NMW_INVALID);
}
Bytes Worker::exchange(const Bytes &request, const NMWOperation &op) {
    if (request.empty() || request.size() > 16 * 1024 * 1024)
        fail(NMW_OVERSIZED);
    const auto deadline = Clock::now() + std::chrono::seconds(65);
    try {
        start(op, deadline);
        Bytes header;
        appendWord(header, static_cast<uint32_t>(request.size()));
        writeAll(child.input, header.data(), 4, op, deadline);
        writeAll(child.input, request.data(), request.size(), op, deadline);
        return readFrame(op, deadline);
    } catch (...) {
        close();
        throw;
    }
}
} // namespace wrapper
