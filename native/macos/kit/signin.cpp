#include "runtime.h"
#include <array>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace wrapper;
struct NMWLogin {
    std::string binary, assets, profile, operation;
    std::mutex mutex;
    NMWOperation cancellation;
    bool started = false, awaiting = false;
    int input = -1;
    NMWLogin(const char *b, const char *a, const char *p, const char *op)
        : binary(b), assets(a), profile(p), operation(op) {}
};
namespace {
void erase(Bytes &buffer) {
    volatile uint8_t *p = buffer.data();
    for (size_t i = 0; i < buffer.size(); ++i)
        p[i] = 0;
}
void run(NMWLogin &state, void *context, NMWAuthPromptCallback callback) {
    PrivateChild child;
    auto deadline = Clock::now() + std::chrono::minutes(10);
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.started)
            fail(NMW_BUSY);
        state.started = true;
        check(state.cancellation);
        std::filesystem::create_directories(state.profile);
        if (chmod(state.profile.c_str(), 0700))
            fail(NMW_UNAVAILABLE);
        child.spawn({state.binary, "--assets", state.assets, "--apple-profile", "--probe", state.operation,
                     "--data-dir", state.profile, "--prompt-script", "@private-pipe", "--owner-pid",
                     std::to_string(getpid())});
        state.input = child.input;
    }
    struct Clear {
        NMWLogin &state;
        ~Clear() {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.input = -1;
            state.awaiting = false;
        }
    } clear{state};
    for (;;) {
        Bytes prefix(4);
        pipeRead(child.output, prefix.data(), prefix.size(), state.cancellation, deadline);
        if (prefix.front() == '{') {
            std::array<uint8_t, 4096> part{};
            for (;;) {
                auto count = pipeReadSome(child.output, part.data(), part.size(), state.cancellation, deadline);
                if (!count)
                    break;
                if (count > maxMetadata - prefix.size())
                    fail(NMW_OVERSIZED);
                prefix.insert(prefix.end(), part.begin(), part.begin() + count);
            }
            auto report = json(prefix);
            if (!report.value("ok", false) || !report.contains("result"))
                fail(NMW_REJECTED);
            const auto &result = report["result"];
            if (result.value("cancelled", false))
                fail(NMW_CANCELLED);
            bool success = result.value("authenticated", false) && result.value("account_present", false);
            if (!success)
                fail(NMW_REJECTED);
            return;
        }
        auto count = word(prefix, 0);
        if (!count || count > maxMetadata)
            fail(NMW_INVALID);
        Bytes prompt(count);
        pipeRead(child.output, prompt.data(), prompt.size(), state.cancellation, deadline);
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.awaiting = true;
        }
        auto code = callback(context, prompt.data(), prompt.size());
        if (code)
            fail(code);
    }
}
} // namespace
extern "C" {
NMWLogin *nmw_login_create(const char *binary, const char *assets, const char *profile, const char *operation) {
    if (!binary || !assets || !profile || !operation)
        return nullptr;
    std::string op = operation;
    if (op != "auth-login" && op != "auth-input-pipe")
        return nullptr;
    try {
        return new NMWLogin(binary, assets, profile, operation);
    } catch (...) {
        return nullptr;
    }
}
int32_t nmw_login_run(NMWLogin *s, void *context, NMWAuthPromptCallback cb) {
    if (!s || !cb)
        return NMW_INVALID;
    try {
        run(*s, context, cb);
        return NMW_OK;
    } catch (const Failure &e) {
        return s->cancellation.cancelled ? NMW_CANCELLED : e.code;
    } catch (...) {
        return NMW_INVALID;
    }
}
void nmw_login_submit(NMWLogin *s, const uint8_t *answer, size_t size) {
    if (!s || !answer || !size || size > 8192)
        return;
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (!s->awaiting || s->cancellation.cancelled || s->input < 0)
            return;
        s->awaiting = false;
        fd = fcntl(s->input, F_DUPFD_CLOEXEC, 3);
    }
    if (fd < 0) {
        s->cancellation.cancelled = true;
        return;
    }
    Bytes frame;
    try {
        appendWord(frame, static_cast<uint32_t>(size));
        frame.insert(frame.end(), answer, answer + size);
        pipeWrite(fd, frame.data(), frame.size(), s->cancellation, Clock::now() + std::chrono::seconds(3));
    } catch (...) {
        s->cancellation.cancelled = true;
    }
    erase(frame);
    ::close(fd);
}
void nmw_login_cancel(NMWLogin *s) {
    if (s)
        s->cancellation.cancelled = true;
}
void nmw_login_destroy(NMWLogin *s) { delete s; }
}
