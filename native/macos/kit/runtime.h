#pragma once
#include "include/NativeMusicWrapperKit.h"
#include "../third_party/json/json.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <sys/types.h>

struct NMWOperation {
    std::atomic<bool> cancelled{false};
};
namespace wrapper {
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr size_t maxBody = 16 * 1024 * 1024 - 8, maxReply = 32 * 1024 * 1024, maxMetadata = 64 * 1024;
struct Failure {
    int code;
    std::string message;
};
[[noreturn]] inline void fail(int code, std::string message = {}) { throw Failure{code, std::move(message)}; }
inline void check(const NMWOperation &operation) {
    if (operation.cancelled)
        fail(NMW_CANCELLED);
}
Bytes bytes(const Json &value);
Json json(const Bytes &value);
std::string field(const Json &value, const char *name, const std::string &fallback = {});
std::string urlHost(const std::string &value, bool appleOnly = false);
bool sameOrigin(const std::string &source, const std::string &next);
bool mayRetryTLSWithSystemDefault(int curlResult, size_t receivedBytes);
bool validID(const std::string &id, bool allowZero = false);
void appendWord(Bytes &out, uint32_t word);
uint32_t word(const Bytes &input, size_t at);
void appendString(Bytes &out, const std::string &s);
std::vector<uint32_t> sampleSizes(const Bytes &input);

class Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool occupied = false;
    std::deque<const NMWOperation *> pending;

  public:
    void acquire(const NMWOperation &operation, bool waitUntilAvailable = false);
    void release();
    size_t waiting() {
        std::lock_guard<std::mutex> guard(mutex);
        return pending.size();
    }
};
class Admission {
    Gate &gate;

  public:
    Admission(Gate &gate, const NMWOperation &operation, bool waitUntilAvailable = false) : gate(gate) {
        gate.acquire(operation, waitUntilAvailable);
    }
    ~Admission() { gate.release(); }
};
struct PrivateChild {
    pid_t pid = -1;
    int input = -1, output = -1;
    PrivateChild() = default;
    PrivateChild(const PrivateChild &) = delete;
    PrivateChild &operator=(const PrivateChild &) = delete;
    ~PrivateChild() { stop(); }
    void spawn(const std::vector<std::string> &arguments);
    void stop();
};
void pipeRead(int fd, uint8_t *destination, size_t count, const NMWOperation &, Clock::time_point);
void pipeWrite(int fd, const uint8_t *source, size_t count, const NMWOperation &, Clock::time_point);
size_t pipeReadSome(int fd, uint8_t *destination, size_t count, const NMWOperation &, Clock::time_point);
class Worker {
    std::string binary, assets, profile;
    PrivateChild child;
    int profileLock = -1;
    int version = 0;
    void start(const NMWOperation &operation, Clock::time_point deadline);
    Bytes readFrame(const NMWOperation &operation, Clock::time_point deadline);

  public:
    Worker(std::string binary, std::string assets, std::string profile)
        : binary(std::move(binary)), assets(std::move(assets)), profile(std::move(profile)) {}
    ~Worker() { close(); }
    Bytes exchange(const Bytes &request, const NMWOperation &operation);
    int generation() const { return version; }
    void close();
};
struct HTTPRequest {
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
};
Bytes https(const HTTPRequest &, const NMWOperation &);
bool appendBounded(Bytes &destination, const char *data, size_t length, size_t limit);
struct Reply {
    Json envelope;
    Bytes binary;
};
class Service {
    Worker worker;
    void *hookContext;
    NMWExternal hook;
    Gate gate, artistGate;
    std::mutex accountLock, developerLock;
    bool changingAccount = false;
    Json tokens;
    Clock::time_point tokenAt{}, developerAt{};
    std::string developer;
    struct Context {
        std::string id, uri, content;
        Clock::time_point used;
        int generation = -1;
        uint32_t handle = 0;
    };
    std::map<std::string, Context> contexts;
    unsigned opens = 0;
    Bytes external(int kind, const Bytes &input, const NMWOperation &);
    Bytes exchange(const Bytes &, const NMWOperation &);
    int generation(const NMWOperation &);
    void close(const NMWOperation &);
    Bytes fetch(const HTTPRequest &, const NMWOperation &);
    std::string webToken(const NMWOperation &);
    Json credentials(const NMWOperation &, bool webRequired = false);
    Json webRoute(const std::string &route, const std::string &id, const Json &query, const Bytes &body,
                  const NMWOperation &);
    Json deviceAsset(const std::string &id, const NMWOperation &);
    void ensure(Context &, const NMWOperation &);
    void prune();
    Reply perform(const std::string &, const Json &, const Bytes &, const std::string &, const NMWOperation &);

  public:
    Service(std::string binary, std::string assets, std::string profile, void *context = nullptr,
            NMWExternal hook = nullptr)
        : worker(std::move(binary), std::move(assets), std::move(profile)), hookContext(context), hook(hook) {}
    Reply send(const std::string &route, const Json &query, const Bytes &body, const std::string &context,
               const NMWOperation &);
    void beginAccount(const NMWOperation &);
    void endAccount();
    void closeWorker();
    size_t pending() { return gate.waiting(); }
};
void copyProfile(const std::string &source, const std::string &destination);
int serve(int argc, char **argv, const std::string &executable);
} // namespace wrapper
