#include "shims.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <strings.h>
#include <unistd.h>
#include <vector>

namespace native_poc {
namespace {

struct Destructor {
    void (*function)(void*);
    void* argument;
    void* dso;
    bool called = false;
};

std::vector<Destructor>& destructors() {
    static std::vector<Destructor> entries;
    return entries;
}

extern "C" int guest_atexit(void (*function)(void*), void* argument, void* dso) {
    destructors().push_back({function, argument, dso, false});
    return 0;
}

extern "C" void guest_finalize(void* dso) {
    // Mark before invoking: a callback can register or finalize other callbacks.
    auto& entries = destructors();
    for (size_t i = entries.size(); i > 0; --i) {
        auto entry = entries[i - 1];
        if (!entry.called && (!dso || entry.dso == dso)) {
            entries[i - 1].called = true;
            entry.function(entry.argument);
        }
    }
}

extern "C" void* guest_memalign(size_t alignment, size_t size) {
    void* result = nullptr;
    return posix_memalign(&result, alignment, size) == 0 ? result : nullptr;
}

[[noreturn]] void guardedCall(const char* symbol) {
    // Do not read guest variadic arguments or return fabricated success.
    std::fputs("native-poc: guarded Android ABI call: ", stderr);
    std::fputs(symbol, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    _exit(86);
}

// These are addresses for known function imports, never substitutes for data.
// Entering any one stops the isolated probe instead of calling an incompatible
// Darwin API (varargs, pthread objects, FILE/stat layouts, errno, or Linux flags).
#define GUARDS(X) \
    X(__assert) X(__assert2) X(__errno) X(__stack_chk_fail) X(__cxa_pure_virtual) \
    X(__android_log_print) X(__android_log_write) X(__android_log_vprint) \
    X(__system_property_get) X(__system_property_find) X(__system_property_read) \
    X(abort) X(arc4random) X(chmod) X(close) X(dlopen) X(dlclose) X(dlsym) X(dlerror) \
    X(fclose) X(fcntl) X(flock) X(fopen) X(fprintf) X(fread) X(fstat) X(ftruncate) \
    X(fwrite) X(gettimeofday) X(lseek) X(lstat) X(mkdir) X(mktime) X(open) X(read) \
    X(rename) X(sleep) X(snprintf) X(strerror) X(syscall) X(umask) X(unlink) \
    X(vsnprintf) X(write) X(pthread_create) X(pthread_kill) X(pthread_once) \
    X(pthread_mutex_lock) X(pthread_mutex_unlock) X(pthread_rwlock_destroy) \
    X(pthread_rwlock_init) X(pthread_rwlock_rdlock) X(pthread_rwlock_unlock) \
    X(pthread_rwlock_wrlock)

#define DEFINE_GUARD(name) extern "C" [[noreturn]] void guard_##name() { guardedCall(#name); }
GUARDS(DEFINE_GUARD)
#undef DEFINE_GUARD

// An ELF canary is a data import. This address remains valid for the process.
uint64_t stack_canary = [] {
    uint64_t value;
    do {
        arc4random_buf(&value, sizeof(value));
        value &= ~uint64_t(0xff); // Retain a NUL byte as in the native canary.
    } while (!value);
    return value;
}();

} // namespace

Bridge bridgeSymbol(const std::string& name) {
#define FUNCTION(symbol) if (name == #symbol) return {::dlsym(RTLD_DEFAULT, #symbol), false, false}
    FUNCTION(malloc); FUNCTION(calloc); FUNCTION(realloc); FUNCTION(free);
    FUNCTION(memcpy); FUNCTION(memmove); FUNCTION(memset); FUNCTION(memcmp); FUNCTION(memchr);
    FUNCTION(strlen); FUNCTION(strnlen); FUNCTION(strcmp); FUNCTION(strncmp);
    FUNCTION(strcpy); FUNCTION(strncpy); FUNCTION(strcat); FUNCTION(strncat);
    FUNCTION(strdup); FUNCTION(strndup); FUNCTION(strchr); FUNCTION(strrchr);
    FUNCTION(strstr); FUNCTION(strlcpy); FUNCTION(strlcat);
    FUNCTION(abs); FUNCTION(labs); FUNCTION(llabs);
#undef FUNCTION
    if (name == "memalign") return {reinterpret_cast<void*>(&guest_memalign), false, false};
    if (name == "__cxa_atexit") return {reinterpret_cast<void*>(&guest_atexit), false, false};
    if (name == "__cxa_finalize") return {reinterpret_cast<void*>(&guest_finalize), false, false};
    if (name == "__stack_chk_guard") return {&stack_canary, false, true};
#define MATCH_GUARD(symbol) if (name == #symbol) return {reinterpret_cast<void*>(&guard_##symbol), true, false};
    GUARDS(MATCH_GUARD)
#undef MATCH_GUARD
    return {};
}

size_t guestDestructorCount() { return destructors().size(); }

} // namespace native_poc
