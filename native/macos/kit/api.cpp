#include "runtime.h"
#include <cstdlib>
#include <cstring>
#include <new>

struct NMWRuntime {
    wrapper::Service service;
};
struct NMWWorker {
    wrapper::Worker value;
    std::mutex lock;
};
namespace {
NMWBytes copy(const wrapper::Bytes &value) { return nmw_copy_bytes(value.data(), value.size()); }
NMWReply error(int code, const std::string &info = {}) {
    return {code, nmw_copy_bytes(reinterpret_cast<const uint8_t *>(info.data()), info.size()), {}};
}
template <class F> int32_t guarded(F func) {
    try {
        func();
        return NMW_OK;
    } catch (const wrapper::Failure &e) {
        return e.code;
    } catch (...) {
        return NMW_INVALID;
    }
}
} // namespace
extern "C" {
NMWRuntime *nmw_create(const char *binary, const char *assets, const char *profile, void *context,
                       NMWExternal hook) {
    if (!binary || !assets || !profile)
        return nullptr;
    try {
        return new NMWRuntime{{binary, assets, profile, context, hook}};
    } catch (...) {
        return nullptr;
    }
}
void nmw_destroy(NMWRuntime *r) { delete r; }
NMWOperation *nmw_operation_create() { return new (std::nothrow) NMWOperation; }
void nmw_operation_cancel(NMWOperation *o) {
    if (o)
        o->cancelled = true;
}
int nmw_operation_cancelled(const NMWOperation *o) { return o && o->cancelled; }
void nmw_operation_destroy(NMWOperation *o) { delete o; }
NMWBytes nmw_copy_bytes(const uint8_t *data, size_t size) {
    if (!size || !data || size > wrapper::maxReply)
        return {};
    auto *p = static_cast<uint8_t *>(std::malloc(size));
    if (!p)
        return {};
    std::memcpy(p, data, size);
    return {p, size};
}
void nmw_free_bytes(NMWBytes b) { std::free(b.bytes); }
NMWReply nmw_send(NMWRuntime *r, NMWOperation *op, const char *route, const char *query, const uint8_t *data,
                  size_t n, const char *context) {
    if (!r || !op || !route || !query || !context || (!data && n))
        return error(NMW_INVALID);
    if (n > wrapper::maxBody || strnlen(query, 128 * 1024 + 1) > 128 * 1024 || strnlen(route, 65) > 64 ||
        strnlen(context, 49) > 48)
        return error(NMW_OVERSIZED);
    try {
        auto q = wrapper::Json::parse(query, nullptr, false);
        if (!q.is_object())
            return error(NMW_INVALID);
        wrapper::Bytes body;
        if (n)
            body.assign(data, data + n);
        auto out = r->service.send(route, q, body, context, *op);
        auto meta = wrapper::bytes(out.envelope);
        if (meta.size() > wrapper::maxReply || out.binary.size() > wrapper::maxReply)
            return error(NMW_OVERSIZED);
        auto metadata = copy(meta), binary = copy(out.binary);
        if (metadata.size != meta.size() || binary.size != out.binary.size()) {
            nmw_free_bytes(metadata);
            nmw_free_bytes(binary);
            return error(NMW_UNAVAILABLE);
        }
        return {NMW_OK, metadata, binary};
    } catch (const wrapper::Failure &e) {
        return error(e.code, e.message);
    } catch (...) {
        return error(NMW_INVALID);
    }
}
int32_t nmw_begin_account(NMWRuntime *r, NMWOperation *o) {
    if (!r || !o)
        return NMW_INVALID;
    return guarded([&] { r->service.beginAccount(*o); });
}
void nmw_end_account(NMWRuntime *r) {
    if (r)
        (void)guarded([&] { r->service.endAccount(); });
}
void nmw_close_worker(NMWRuntime *r) {
    if (r)
        (void)guarded([&] { r->service.closeWorker(); });
}
size_t nmw_pending_requests(NMWRuntime *r) { return r ? r->service.pending() : 0; }
int32_t nmw_import_profile(const char *source, const char *dest) {
    if (!source || !dest)
        return NMW_INVALID;
    return guarded([&] { wrapper::copyProfile(source, dest); });
}
int32_t nmw_sample_sizes(const uint8_t *data, size_t size, NMWBytes *out) {
    if (!out || !data || size > wrapper::maxBody)
        return NMW_INVALID;
    *out = {};
    return guarded([&] {
        auto sizes = wrapper::sampleSizes(wrapper::Bytes(data, data + size));
        wrapper::Bytes encoded;
        for (auto n : sizes)
            wrapper::appendWord(encoded, n);
        *out = copy(encoded);
        if (out->size != encoded.size())
            wrapper::fail(NMW_UNAVAILABLE);
    });
}
int nmw_permits_redirect(const char *from, const char *to) {
    if (!from || !to)
        return 0;
    try {
        return wrapper::sameOrigin(from, to);
    } catch (...) {
        return 0;
    }
}
NMWWorker *nmw_worker_create(const char *path, const char *assets, const char *profile) {
    if (!path || !assets || !profile)
        return nullptr;
    try {
        return new NMWWorker{{path, assets, profile}, {}};
    } catch (...) {
        return nullptr;
    }
}
NMWReply nmw_worker_exchange(NMWWorker *w, NMWOperation *op, const uint8_t *data, size_t size) {
    if (!w || !op || !data || size > 16 * 1024 * 1024)
        return error(NMW_INVALID);
    try {
        std::lock_guard<std::mutex> l(w->lock);
        auto result = w->value.exchange(wrapper::Bytes(data, data + size), *op);
        auto b = copy(result);
        if (b.size != result.size())
            return error(NMW_UNAVAILABLE);
        return {NMW_OK, b, {}};
    } catch (const wrapper::Failure &e) {
        return error(e.code, e.message);
    } catch (...) {
        return error(NMW_INVALID);
    }
}
void nmw_worker_close(NMWWorker *w) {
    if (w) {
        std::lock_guard<std::mutex> l(w->lock);
        w->value.close();
    }
}
void nmw_worker_destroy(NMWWorker *w) { delete w; }
}
