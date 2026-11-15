#include "runtime.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace wrapper {
Bytes bytes(const Json &j) {
    auto s = j.dump();
    return Bytes(s.begin(), s.end());
}
Json json(const Bytes &data) {
    auto result = Json::parse(data.begin(), data.end(), nullptr, false);
    if (result.is_discarded())
        fail(NMW_INVALID);
    return result;
}
std::string field(const Json &j, const char *key, const std::string &fallback) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}
bool validID(const std::string &id, bool allowZero) {
    if (id.empty() || id.size() > 19 ||
        !std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    int64_t v = 0;
    auto p = std::from_chars(id.data(), id.data() + id.size(), v);
    return p.ec == std::errc{} && (v > 0 || (allowZero && id == "0"));
}
void appendWord(Bytes &dest, uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8)
        dest.push_back(static_cast<uint8_t>(n >> shift));
}
void appendString(Bytes &out, const std::string &s) {
    appendWord(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}
uint32_t word(const Bytes &data, size_t at) {
    if (at > data.size() || data.size() - at < 4)
        fail(NMW_INVALID);
    return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) |
           data[at + 3];
}
std::vector<uint32_t> sampleSizes(const Bytes &data) {
    if (data.size() > maxBody)
        fail(NMW_OVERSIZED);
    auto n = word(data, 0);
    if (!n || n > 8192)
        fail(NMW_INVALID);
    std::vector<uint32_t> sizes;
    sizes.reserve(n);
    size_t offset = 4;
    for (uint32_t i = 0; i < n; ++i) {
        auto size = word(data, offset);
        offset += 4;
        if (!size || size > 4 * 1024 * 1024 || size > data.size() - offset)
            fail(NMW_INVALID);
        sizes.push_back(size);
        offset += size;
    }
    if (offset != data.size())
        fail(NMW_INVALID);
    return sizes;
}
void Gate::acquire(const NMWOperation &op, bool waitUntilAvailable) {
    check(op);
    std::unique_lock<std::mutex> guard(mutex);
    if (!occupied && pending.empty()) {
        occupied = true;
        return;
    }
    if (!waitUntilAvailable && pending.size() >= 7)
        fail(NMW_BUSY);
    pending.push_back(&op);
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    for (;;) {
        if (op.cancelled || (!waitUntilAvailable && Clock::now() >= deadline)) {
            pending.erase(std::find(pending.begin(), pending.end(), &op));
            changed.notify_all();
            fail(op.cancelled ? NMW_CANCELLED : NMW_BUSY);
        }
        if (!occupied && pending.front() == &op) {
            pending.pop_front();
            occupied = true;
            return;
        }
        changed.wait_for(guard, std::chrono::milliseconds(25));
    }
}
void Gate::release() {
    std::lock_guard<std::mutex> guard(mutex);
    occupied = false;
    changed.notify_all();
}
namespace {
Bytes opcode(uint32_t value) {
    Bytes data;
    appendWord(data, value);
    return data;
}
Reply reply(Json data, int code = 0, const std::string &msg = "success") {
    return {Json{{"code", code}, {"msg", msg}, {"data", std::move(data)}}, {}};
}
bool printable(const std::string &s) {
    return std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool contextToken(const std::string &token) {
    return token.size() == 48 && std::all_of(token.begin(), token.end(), [](char c) {
               return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
           });
}
std::string newToken() {
    unsigned char raw[24];
    arc4random_buf(raw, sizeof(raw));
    std::string s;
    s.reserve(48);
    const char *digits = "0123456789abcdef";
    for (auto b : raw) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}
} // namespace
Bytes Service::external(int kind, const Bytes &input, const NMWOperation &op) {
    check(op);
    NMWBytes result{};
    int code = hook(hookContext, kind, &op, input.data(), input.size(), &result);
    struct Release {
        NMWBytes b;
        ~Release() { nmw_free_bytes(b); }
    } release{result};
    check(op);
    if (code != NMW_OK) {
        std::string message;
        if (code == NMW_MESSAGE && result.bytes && result.size <= 1024)
            message.assign(reinterpret_cast<char *>(result.bytes), result.size);
        fail(code, message);
    }
    if (result.size > maxReply || (result.size && !result.bytes))
        fail(NMW_INVALID);
    return result.size ? Bytes(result.bytes, result.bytes + result.size) : Bytes{};
}
Bytes Service::exchange(const Bytes &input, const NMWOperation &op) {
    return hook ? external(NMW_WORKER, input, op) : worker.exchange(input, op);
}
int Service::generation(const NMWOperation &op) {
    return hook ? static_cast<int>(word(external(NMW_GENERATION, {}, op), 0)) : worker.generation();
}
void Service::close(const NMWOperation &op) {
    if (hook)
        (void)external(NMW_CLOSE_WORKER, {}, op);
    else
        worker.close();
}
void Service::prune() {
    auto cutoff = Clock::now() - std::chrono::minutes(30);
    for (auto it = contexts.begin(); it != contexts.end();) {
        if (it->second.used < cutoff)
            it = contexts.erase(it);
        else
            ++it;
    }
    while (contexts.size() > 32) {
        auto oldest = std::min_element(contexts.begin(), contexts.end(),
                                       [](const auto &a, const auto &b) { return a.second.used < b.second.used; });
        contexts.erase(oldest);
    }
}
void Service::ensure(Context &entry, const NMWOperation &op) {
    exchange(opcode(4), op);
    if (entry.generation == generation(op))
        return;
    if (opens >= 64) {
        close(op);
        opens = 0;
    }
    exchange(opcode(3), op);
    auto request = opcode(1);
    appendString(request, entry.id);
    appendString(request, entry.uri);
    auto response = exchange(request, op);
    ++opens;
    auto handle = word(response, 0), count = word(response, 4);
    if (!handle || handle > 64 || !count || response.size() != 8 + size_t(count))
        fail(NMW_INVALID);
    entry.handle = handle;
    entry.generation = generation(op);
    entry.content.assign(response.begin() + 8, response.end());
}
Json Service::deviceAsset(const std::string &id, const NMWOperation &op) {
    auto request = opcode(6);
    appendString(request, id);
    auto raw = exchange(request, op);
    if (raw.size() > maxMetadata)
        fail(NMW_OVERSIZED);
    auto result = json(raw);
    if (field(result, "adamId") != id || !result.contains("m3u8"))
        fail(NMW_INVALID);
    if (result["m3u8"].is_null())
        return nullptr;
    auto type = field(result, "streamType");
    if ((type != "hls" && type != "file") || urlHost(field(result, "m3u8"), true).empty())
        fail(NMW_INVALID);
    return result;
}
Reply Service::perform(const std::string &route, const Json &q, const Bytes &body, const std::string &token,
                       const NMWOperation &op) {
    if (route == "/status") {
        auto status = json(exchange(opcode(4), op));
        if (!status.is_object())
            fail(NMW_INVALID);
        status["transport"] = "embedded";
        return reply(status);
    }
    if (route == "/decrypt") {
        auto sizes = sampleSizes(body);
        prune();
        auto it = contexts.find(token);
        if (!contextToken(token) || it == contexts.end())
            return reply(nullptr, 409, "Native context expired; request the key again.");
        ensure(it->second, op);
        auto request = opcode(2);
        appendWord(request, it->second.handle);
        request.insert(request.end(), body.begin(), body.end());
        auto clear = exchange(request, op);
        try {
            if (sampleSizes(clear) != sizes)
                fail(NMW_INVALID);
        } catch (...) {
            close(op);
            throw;
        }
        it->second.used = Clock::now();
        auto output = reply(nullptr);
        output.binary = std::move(clear);
        return output;
    }
    std::string id = field(q, "adamId");
    if (route == "/license")
        return reply(webRoute(route, {}, q, body, op));
    if (!validID(id, route == "/key")) {
        if (route == "/artist")
            return reply(nullptr, 400, "invalid adamId");
        fail(NMW_MESSAGE, "Invalid catalog track ID.");
    }
    if (route == "/m3u8") {
        auto asset = deviceAsset(id, op);
        if (asset.is_null())
            return reply(nullptr, 404, "failed to get m3u8");
        // Raw clients always receive the SDK-selected asset. Only clients
        // requiring HLS request the CBC stream for a direct protected file.
        if (field(q, "representation") == "hls" && field(asset, "streamType") == "file")
            return reply(webRoute("/assets", id, q, {}, op));
        return reply(asset);
    }
    if (route == "/key") {
        auto uri = field(q, "uri");
        if (uri.rfind("skd://", 0) != 0 || uri.size() > 8192 || !printable(uri) ||
            (id != "0" && uri == "skd://itunes.apple.com/P000000000/s1/e1"))
            fail(NMW_MESSAGE, "Invalid native key URI.");
        prune();
        auto it = std::find_if(contexts.begin(), contexts.end(),
                               [&](const auto &item) { return item.second.id == id && item.second.uri == uri; });
        std::string key = it == contexts.end() ? newToken() : it->first;
        Context entry = it == contexts.end() ? Context{id, uri, {}, Clock::now()} : it->second;
        ensure(entry, op);
        entry.used = Clock::now();
        contexts[key] = entry;
        prune();
        return reply({{"adamId", id},
                      {"keyUri", uri},
                      {"contentKey", entry.content},
                      {"nativeDecryptionVersion", 1},
                      {"nativeContext", key}});
    }
    if (route == "/artist") {
        auto region = field(q, "storefront", "us");
        if (region.size() != 2 || !std::all_of(region.begin(), region.end(), [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            }))
            return reply(nullptr, 400, "invalid storefront");
        auto artist = webRoute(route, std::to_string(std::stoll(id)), q, {}, op);
        return artist.is_null() ? reply(nullptr, 404, "artist not found") : reply(std::move(artist));
    }
    if (route == "/lyrics" || route == "/webplayback" || route == "/assets")
        return reply(webRoute(route, id, q, body, op));
    fail(NMW_MESSAGE, "Unknown native music operation.");
}
Reply Service::send(const std::string &route, const Json &q, const Bytes &body, const std::string &token,
                    const NMWOperation &op) {
    if (body.size() > maxBody || token.size() > 48 || !q.is_object() || q.size() > 8)
        fail(NMW_OVERSIZED);
    for (const auto &item : q.items())
        if (item.key().size() > 64 || !item.value().is_string() ||
            item.value().get_ref<const std::string &>().size() > 8192)
            fail(NMW_OVERSIZED);
    const bool artist = route == "/artist";
    auto accountAvailable = [&] {
        std::lock_guard<std::mutex> l(accountLock);
        if (!artist && changingAccount)
            fail(NMW_BUSY);
    };
    accountAvailable();
    Admission acquired(artist ? artistGate : gate, op);
    accountAvailable();
    check(op);
    Reply result;
    try {
        result = perform(route, q, body, token, op);
    } catch (const Failure &error) {
        check(op);
        if (error.code == NMW_NO_ACCOUNT)
            return reply(nullptr, 401, "No saved account or account tokens; sign in first.");
        if (error.code == NMW_NOT_FOUND)
            return reply(nullptr, 404, error.message);
        throw;
    }
    check(op);
    return result;
}
void Service::beginAccount(const NMWOperation &op) {
    {
        std::lock_guard<std::mutex> lock(accountLock);
        if (changingAccount)
            fail(NMW_BUSY);
        changingAccount = true;
    }
    try {
        Admission held(gate, op);
        close(op);
    } catch (...) {
        std::lock_guard<std::mutex> lock(accountLock);
        changingAccount = false;
        throw;
    }
}
void Service::endAccount() {
    NMWOperation op;
    Admission held(gate, op);
    tokens = Json{};
    contexts.clear();
    opens = 0;
    {
        std::lock_guard<std::timed_mutex> web(developerLock);
        developer.clear();
    }
    std::lock_guard<std::mutex> lock(accountLock);
    changingAccount = false;
}
void Service::closeWorker() {
    NMWOperation op;
    Admission held(gate, op, true);
    close(op);
}
} // namespace wrapper
