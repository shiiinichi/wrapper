#include "abi_runtime.h"
#include "format_bridge.h"
#include "elf_loader.h"
#include "posix_bridge.h"
#include "network_bridge.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <list>
#include <pthread.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <locale.h>
#include <xlocale.h>
#include <CommonCrypto/CommonDigest.h>

namespace native_poc {
namespace {
constexpr size_t kFileSize = 152;
thread_local int android_errno = 0;
int linuxErrorInternal(int value) {
    if (value == EAGAIN) return 11;
    if (value == EDEADLK) return 35;
    if (value == ETIMEDOUT) return 110;
    if (value == ENOTSUP) return 95;
    if (value == EOVERFLOW) return 75;
    return value;
}
alignas(8) std::array<unsigned char, kFileSize * 3> standard_files{};
std::map<void*, FILE*> files;
std::mutex file_map_lock;
std::vector<std::unique_ptr<std::array<unsigned char, kFileSize>>> file_shells;

FILE* file(void* pointer) {
    intptr_t delta = reinterpret_cast<intptr_t>(pointer) - reinterpret_cast<intptr_t>(standard_files.data());
    if (delta >= 0 && delta < intptr_t(standard_files.size()) && delta % kFileSize == 0)
        return delta == 0 ? stdin : delta == kFileSize ? stdout : stderr;
    std::lock_guard<std::mutex> lock(file_map_lock);
    auto found = files.find(pointer);
    if (found == files.end()) { std::fputs("native-poc: unknown guest FILE\n", stderr); _exit(86); }
    return found->second;
}
void* fopenBridge(const char* path, const char* mode) {
    if (!path || !mode) { android_errno = 22; return nullptr; }
    if (std::strpbrk(mode, "wa+") && !writablePath(path)) { android_errno = 13; return nullptr; }
    FILE* value = std::fopen(path, mode);
    if (!value) { android_errno = linuxError(errno); return nullptr; }
    auto shell = std::make_unique<std::array<unsigned char, kFileSize>>();
    shell->fill(0);
    int flags = std::strchr(mode, '+') ? 0x10 : *mode == 'r' ? 4 : 8, descriptor = fileno(value);
    std::memcpy(shell->data() + 16, &flags, 4); std::memcpy(shell->data() + 20, &descriptor, 4);
    std::lock_guard<std::mutex> lock(file_map_lock);
    void* key = shell->data(); files[key] = value; file_shells.push_back(std::move(shell));
    return key;
}
int fcloseBridge(void* pointer) { FILE* value = file(pointer); { std::lock_guard<std::mutex> lock(file_map_lock); files.erase(pointer); } int result = std::fclose(value); if (result) android_errno = linuxError(errno); return result; }
size_t freadBridge(void* data, size_t size, size_t count, void* pointer) { auto* stream = file(pointer); auto result = std::fread(data, size, count, stream); if (std::ferror(stream)) android_errno = linuxError(errno); return result; }
size_t fwriteBridge(const void* data, size_t size, size_t count, void* pointer) { auto* stream = file(pointer); auto result = std::fwrite(data, size, count, stream); if (std::ferror(stream)) android_errno = linuxError(errno); return result; }
int getcBridge(void* pointer) { auto* stream = file(pointer); int result = std::getc(stream); if (result == EOF && std::ferror(stream)) android_errno = linuxError(errno); return result; }
int ungetcBridge(int value, void* pointer) { return std::ungetc(value, file(pointer)); }
int fputcBridge(int value, void* pointer) { int result = std::fputc(value, file(pointer)); if (result == EOF) android_errno = linuxError(errno); return result; }
int fflushBridge(void* pointer) { int result = std::fflush(pointer ? file(pointer) : nullptr); if (result) android_errno = linuxError(errno); return result; }
int feofBridge(void* pointer) { return std::feof(file(pointer)); }
int ferrorBridge(void* pointer) { return std::ferror(file(pointer)); }
int filenoBridge(void* pointer) { return fileno(file(pointer)); }

struct Mutex {
    pthread_mutex_t value;
    bool live = true;
    explicit Mutex(int type) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr); pthread_mutexattr_settype(&attr, type);
        pthread_mutex_init(&value, &attr); pthread_mutexattr_destroy(&attr);
    }
    ~Mutex() { if (live) pthread_mutex_destroy(&value); }
};
std::mutex mutex_map_lock;
std::map<void*, std::unique_ptr<Mutex>> mutexes;
std::map<void*, int> mutex_attributes;
std::map<void*, std::unique_ptr<std::once_flag>> once_flags;
struct RwLock { pthread_rwlock_t value; bool live = true; RwLock() { pthread_rwlock_init(&value, nullptr); } ~RwLock() { if (live) pthread_rwlock_destroy(&value); } };
std::map<void*, std::unique_ptr<RwLock>> rwlocks;
RwLock& rwlock(void* key) {
    std::lock_guard<std::mutex> lock(mutex_map_lock); auto& value = rwlocks[key];
    if (!value) value = std::make_unique<RwLock>(); return *value;
}
int rwInit(void* key, const void* attr) { if (attr) return 95; (void)rwlock(key); return 0; }
int rwRead(void* key) { return linuxError(pthread_rwlock_rdlock(&rwlock(key).value)); }
int rwWrite(void* key) { return linuxError(pthread_rwlock_wrlock(&rwlock(key).value)); }
int rwUnlock(void* key) { return linuxError(pthread_rwlock_unlock(&rwlock(key).value)); }
int rwDestroy(void* key) {
    std::lock_guard<std::mutex> lock(mutex_map_lock); auto found = rwlocks.find(key); if (found == rwlocks.end()) return 0;
    int result = pthread_rwlock_destroy(&found->second->value);
    if (!result) { found->second->live = false; rwlocks.erase(found); } return linuxError(result);
}
int onceBridge(void* key, void (*callback)()) {
    std::once_flag* flag;
    { std::lock_guard<std::mutex> lock(mutex_map_lock); auto& item = once_flags[key]; if (!item) item = std::make_unique<std::once_flag>(); flag = item.get(); }
    std::call_once(*flag, callback); return 0;
}
Mutex& mutex(void* key) {
    std::lock_guard<std::mutex> lock(mutex_map_lock);
    auto& value = mutexes[key];
    if (!value) {
        uint32_t initial = 0; std::memcpy(&initial, key, 4);
        int type = initial & 0x4000 ? PTHREAD_MUTEX_RECURSIVE :
            initial & 0x8000 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
        value = std::make_unique<Mutex>(type);
    }
    return *value;
}
int mutexLock(void* key) { return linuxError(pthread_mutex_lock(&mutex(key).value)); }
int mutexUnlock(void* key) { return linuxError(pthread_mutex_unlock(&mutex(key).value)); }
int mutexTry(void* key) { return linuxError(pthread_mutex_trylock(&mutex(key).value)); }
int mutexInit(void* key, const void* attr) {
    std::lock_guard<std::mutex> lock(mutex_map_lock);
    int type = PTHREAD_MUTEX_NORMAL;
    if (attr) {
        auto found = mutex_attributes.find(const_cast<void*>(attr));
        if (found == mutex_attributes.end()) return EINVAL;
        type = found->second;
    }
    mutexes[key] = std::make_unique<Mutex>(type); return 0;
}
int mutexDestroy(void* key) {
    std::lock_guard<std::mutex> lock(mutex_map_lock);
    auto found = mutexes.find(key);
    if (found == mutexes.end()) return 0;
    int result = pthread_mutex_destroy(&found->second->value);
    if (!result) { found->second->live = false; mutexes.erase(found); }
    return result;
}
int mutexAttrInit(void* key) { std::lock_guard<std::mutex> lock(mutex_map_lock); mutex_attributes[key] = PTHREAD_MUTEX_NORMAL; return 0; }
int mutexAttrDestroy(void* key) { std::lock_guard<std::mutex> lock(mutex_map_lock); mutex_attributes.erase(key); return 0; }
int mutexAttrType(void* key, int type) {
    if (type < 0 || type > 2) return EINVAL;
    std::lock_guard<std::mutex> lock(mutex_map_lock);
    if (!mutex_attributes.count(key)) return EINVAL;
    mutex_attributes[key] = type == 1 ? PTHREAD_MUTEX_RECURSIVE : type == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
    return 0;
}
struct Condition { pthread_cond_t value; bool live = true; Condition() { pthread_cond_init(&value, nullptr); } ~Condition() { if (live) pthread_cond_destroy(&value); } };
std::map<void*, std::unique_ptr<Condition>> conditions;
Condition& condition(void* key) {
    std::lock_guard<std::mutex> lock(mutex_map_lock);
    auto& value = conditions[key]; if (!value) value = std::make_unique<Condition>(); return *value;
}
int conditionBroadcast(void* key) { return pthread_cond_broadcast(&condition(key).value); }
int conditionSignal(void* key) { return pthread_cond_signal(&condition(key).value); }
int conditionWait(void* key, void* lock) { return pthread_cond_wait(&condition(key).value, &mutex(lock).value); }
int conditionTimedwait(void* key, void* lock, const timespec* timeout) {
    int result = pthread_cond_timedwait(&condition(key).value, &mutex(lock).value, timeout);
    return result == ETIMEDOUT ? 110 : result;
}
int conditionInit(void* key, const void* attr) { if (attr) return 95; (void)condition(key); return 0; }
int conditionDestroy(void* key) {
    std::lock_guard<std::mutex> lock(mutex_map_lock); auto found = conditions.find(key); if (found == conditions.end()) return 0;
    int result = pthread_cond_destroy(&found->second->value);
    if (!result) { found->second->live = false; conditions.erase(found); } return linuxError(result);
}
std::map<uint32_t, pthread_key_t> keys;
std::mutex key_map_lock;
uint32_t next_key = 1;
int keyCreate(uint32_t* output, void (*destructor)(void*)) {
    std::lock_guard<std::mutex> lock(key_map_lock);
    pthread_key_t native;
    int error = pthread_key_create(&native, destructor);
    if (!error) { *output = next_key++; keys[*output] = native; }
    return error;
}
int keyDelete(uint32_t key) { std::lock_guard<std::mutex> lock(key_map_lock); auto i = keys.find(key); if (i == keys.end()) return EINVAL; int e = pthread_key_delete(i->second); keys.erase(i); return e; }
void* keyGet(uint32_t key) { std::lock_guard<std::mutex> lock(key_map_lock); auto i = keys.find(key); return i == keys.end() ? nullptr : pthread_getspecific(i->second); }
int keySet(uint32_t key, const void* value) { std::lock_guard<std::mutex> lock(key_map_lock); auto i = keys.find(key); return i == keys.end() ? EINVAL : pthread_setspecific(i->second, value); }
struct ThreadAttribute {
    pthread_attr_t value;
    ThreadAttribute() { pthread_attr_init(&value); }
    ~ThreadAttribute() { pthread_attr_destroy(&value); }
};
std::mutex attribute_lock;
std::map<void*, std::unique_ptr<ThreadAttribute>> thread_attributes;
int threadAttrInit(void* key) { std::lock_guard<std::mutex> lock(attribute_lock); thread_attributes[key] = std::make_unique<ThreadAttribute>(); return 0; }
int threadAttrDestroy(void* key) { std::lock_guard<std::mutex> lock(attribute_lock); return thread_attributes.erase(key) ? 0 : 22; }
int threadAttrStack(void* key, size_t size) {
    std::lock_guard<std::mutex> lock(attribute_lock); auto i = thread_attributes.find(key);
    return i == thread_attributes.end() ? 22 : linuxError(pthread_attr_setstacksize(&i->second->value, size));
}
int threadAttrDetach(void* key, int state) {
    std::lock_guard<std::mutex> lock(attribute_lock); auto i = thread_attributes.find(key);
    if (i == thread_attributes.end() || (state != 0 && state != 1)) return 22;
    return linuxError(pthread_attr_setdetachstate(&i->second->value, state ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE));
}
int threadCreate(pthread_t* output, const void* attr, void* (*callback)(void*), void* argument) {
    std::lock_guard<std::mutex> lock(attribute_lock);
    const pthread_attr_t* native = nullptr;
    if (attr) { auto i = thread_attributes.find(const_cast<void*>(attr)); if (i == thread_attributes.end()) return 22; native = &i->second->value; }
    return linuxError(pthread_create(output, native, callback, argument));
}
int threadJoin(pthread_t thread, void** result) { return linuxError(pthread_join(thread, result)); }
int threadDetach(pthread_t thread) { return linuxError(pthread_detach(thread)); }
int threadName(pthread_t thread, const char* name) { return thread == pthread_self() ? linuxError(pthread_setname_np(name)) : 95; }
int propertyGet(const char*, char* value) { if (value) *value = 0; return 0; }
int* errnoBridge() { return &android_errno; }
struct FutexWaiter { std::condition_variable condition; bool awake = false; };
struct Futex { std::mutex mutex; std::list<FutexWaiter*> waiters; };
std::mutex futex_map_lock;
std::map<uintptr_t, std::unique_ptr<Futex>> futexes;
long syscallBridge(long number, uintptr_t address, uintptr_t operation, uintptr_t value,
                   uintptr_t timeout, uintptr_t, uintptr_t) {
    if (number == 178) { uint64_t id = 0; pthread_threadid_np(nullptr, &id); return static_cast<long>(id); }
    if (number != 98) { std::fprintf(stderr, "native-poc: unsupported Linux syscall %ld\n", number); _exit(86); }
    Futex* futex;
    { std::lock_guard<std::mutex> lock(futex_map_lock); auto& f = futexes[address]; if (!f) f = std::make_unique<Futex>(); futex = f.get(); }
    std::unique_lock<std::mutex> lock(futex->mutex);
    unsigned command = operation & 127;
    if (command == 1) {
        long count = 0;
        while (!futex->waiters.empty() && count < static_cast<long>(value)) {
            auto* waiter = futex->waiters.front(); futex->waiters.pop_front();
            waiter->awake = true; waiter->condition.notify_one(); ++count;
        }
        return count;
    }
    if (command == 0) {
        if (__atomic_load_n(reinterpret_cast<int*>(address), __ATOMIC_SEQ_CST) != static_cast<int>(value)) {
            android_errno = 11; return -1;
        }
        FutexWaiter waiter; futex->waiters.push_back(&waiter);
        bool woke = true;
        if (timeout) {
            auto* duration = reinterpret_cast<const int64_t*>(timeout);
            woke = waiter.condition.wait_for(lock, std::chrono::seconds(duration[0]) + std::chrono::nanoseconds(duration[1]), [&] { return waiter.awake; });
        } else waiter.condition.wait(lock, [&] { return waiter.awake; });
        if (!woke) { futex->waiters.remove(&waiter); android_errno = 110; return -1; }
        return 0;
    }
    std::fprintf(stderr, "native-poc: unsupported futex command %u\n", command); _exit(86);
}
long timezone_value = 0;
char utc[] = "UTC";
char* timezone_names[] = {utc, utc};
char* option_argument = nullptr;
size_t multibyteMax() { return MB_CUR_MAX; }
locale_t newlocaleBridge(int mask, const char* name, locale_t base) {
    int native_mask = 0;
    const int categories[] = {LC_CTYPE_MASK, LC_NUMERIC_MASK, LC_TIME_MASK, LC_COLLATE_MASK, LC_MONETARY_MASK, LC_MESSAGES_MASK};
    for (unsigned i = 0; i < 6; ++i) if (mask & (1 << i)) native_mask |= categories[i];
    if (mask & ~63) { android_errno = 22; return nullptr; }
    return newlocale(native_mask, name, base);
}
size_t strlenChecked(const char* value, size_t size) { size_t n = strnlen(value, size); if (n == size) std::abort(); return n; }
void* memmoveChecked(void* a, const void* b, size_t n, size_t size) { if (n > size) std::abort(); return std::memmove(a, b, n); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
unsigned char* sha1Bridge(const void* bytes, size_t size, unsigned char* output) {
    thread_local std::array<unsigned char, CC_SHA1_DIGEST_LENGTH> fallback{};
    if (!output) output = fallback.data();
    CC_SHA1_CTX context; CC_SHA1_Init(&context);
    auto* pointer = static_cast<const unsigned char*>(bytes);
    while (size) { auto chunk = static_cast<CC_LONG>(std::min(size, size_t(UINT32_MAX))); CC_SHA1_Update(&context, pointer, chunk); size -= chunk; pointer += chunk; }
    CC_SHA1_Final(output, &context); return output;
}
#pragma clang diagnostic pop

Loader* runtime_loader = nullptr;
thread_local std::string dynamic_error;
void* dlopenBridge(const char* path, int flags) {
    if (!runtime_loader || !path || (flags & ~(1 | 2 | 0x100))) { dynamic_error = "unsupported guest dlopen request"; return nullptr; }
    auto name = std::filesystem::path(path).filename().string();
    for (const auto& image : runtime_loader->images()) if (image->report.library == name) return image.get();
    dynamic_error = "guest dlopen supports only images already loaded: " + name; return nullptr;
}
void* dlsymBridge(void* handle, const char* name) {
    if (!runtime_loader || !name) return nullptr;
    for (const auto& image : runtime_loader->images()) {
        if (!handle || handle == image.get()) if (auto* result = image->exported(name)) return result;
    }
    dynamic_error = "guest symbol is not exported: " + std::string(name); return nullptr;
}
int dlcloseBridge(void* handle) {
    if (runtime_loader) for (const auto& image : runtime_loader->images()) if (handle == image.get()) return 0;
    dynamic_error = "unknown guest dlclose handle"; return -1;
}
const char* dlerrorBridge() {
    thread_local std::string result;
    result = std::move(dynamic_error); dynamic_error.clear(); return result.empty() ? nullptr : result.c_str();
}

struct Guard { std::string name; void* address; };
std::vector<std::unique_ptr<Guard>> guards;
uint8_t* guard_memory = nullptr;
size_t guard_size = 0, guard_used = 0;
bool guard_finished = false;
[[noreturn]] void unknownFunction(const char* name) {
    std::fputs("native-poc: unimplemented Android function: ", stderr); std::fputs(name, stderr); std::fputc('\n', stderr);
    std::fflush(stderr); _exit(86);
}

} // namespace

void bindRuntimeLoader(Loader* loader) { runtime_loader = loader; }
int& guestErrno() { return android_errno; }
int linuxError(int value) { return linuxErrorInternal(value); }

Bridge runtimeBridge(const std::string& name) {
    if (auto formatted = formatBridge(name); formatted.address) return formatted;
    if (auto posix = posixBridge(name); posix.address) return posix;
    if (auto network = networkBridge(name); network.address) return network;
#define BRIDGE(symbol, function) if (name == symbol) return {reinterpret_cast<void*>(&function), false, false}
    BRIDGE("fopen", fopenBridge); BRIDGE("fclose", fcloseBridge); BRIDGE("fread", freadBridge); BRIDGE("fwrite", fwriteBridge);
    BRIDGE("getc", getcBridge); BRIDGE("ungetc", ungetcBridge); BRIDGE("fputc", fputcBridge); BRIDGE("fflush", fflushBridge);
    BRIDGE("feof", feofBridge); BRIDGE("ferror", ferrorBridge); BRIDGE("fileno", filenoBridge);
    BRIDGE("pthread_mutex_lock", mutexLock); BRIDGE("pthread_mutex_unlock", mutexUnlock); BRIDGE("pthread_mutex_trylock", mutexTry);
    BRIDGE("pthread_mutex_init", mutexInit); BRIDGE("pthread_mutex_destroy", mutexDestroy);
    BRIDGE("pthread_mutexattr_init", mutexAttrInit); BRIDGE("pthread_mutexattr_destroy", mutexAttrDestroy); BRIDGE("pthread_mutexattr_settype", mutexAttrType);
    BRIDGE("pthread_cond_init", conditionInit); BRIDGE("pthread_cond_destroy", conditionDestroy);
    BRIDGE("pthread_cond_broadcast", conditionBroadcast); BRIDGE("pthread_cond_signal", conditionSignal);
    BRIDGE("pthread_cond_wait", conditionWait); BRIDGE("pthread_cond_timedwait", conditionTimedwait);
    BRIDGE("pthread_key_create", keyCreate); BRIDGE("pthread_key_delete", keyDelete); BRIDGE("pthread_getspecific", keyGet); BRIDGE("pthread_setspecific", keySet);
    BRIDGE("pthread_create", threadCreate); BRIDGE("pthread_join", threadJoin); BRIDGE("pthread_detach", threadDetach); BRIDGE("pthread_setname_np", threadName);
    BRIDGE("pthread_attr_init", threadAttrInit); BRIDGE("pthread_attr_destroy", threadAttrDestroy);
    BRIDGE("pthread_attr_setstacksize", threadAttrStack); BRIDGE("pthread_attr_setdetachstate", threadAttrDetach);
    BRIDGE("__system_property_get", propertyGet); BRIDGE("__errno", errnoBridge);
    BRIDGE("syscall", syscallBridge);
    BRIDGE("SHA1", sha1Bridge);
    BRIDGE("pthread_once", onceBridge);
    BRIDGE("pthread_rwlock_init", rwInit); BRIDGE("pthread_rwlock_rdlock", rwRead); BRIDGE("pthread_rwlock_wrlock", rwWrite);
    BRIDGE("pthread_rwlock_unlock", rwUnlock); BRIDGE("pthread_rwlock_destroy", rwDestroy);
    BRIDGE("dlopen", dlopenBridge); BRIDGE("dlsym", dlsymBridge); BRIDGE("dlclose", dlcloseBridge); BRIDGE("dlerror", dlerrorBridge);
    BRIDGE("__ctype_get_mb_cur_max", multibyteMax); BRIDGE("__strlen_chk", strlenChecked); BRIDGE("__memmove_chk", memmoveChecked);
    BRIDGE("newlocale", newlocaleBridge);
#undef BRIDGE
    if (name == "__sF") {
        for (int i = 0; i < 3; ++i) { int flags = i == 0 ? 4 : 8; std::memcpy(standard_files.data() + i*kFileSize + 16, &flags, 4); std::memcpy(standard_files.data()+i*kFileSize+20,&i,4); }
        return {standard_files.data(), false, true};
    }
    if (name == "optarg") return {&option_argument, false, true};
    if (name == "timezone") return {&timezone_value, false, true};
    if (name == "tzname") return {timezone_names, false, true};
    // Fixed-argument C APIs with matching scalar/opaque-pointer representations.
    const std::string supported = "|arc4random|getpagesize|getpid|getuid|geteuid|getgid|getegid|getenv|qsort|bsearch|posix_memalign|sched_yield|"
        "pthread_self|pthread_equal|atof|atoi|atol|atoll|strtol|strtoul|strtoll|strtoull|strtod|strtof|"
        "strcasecmp|strncasecmp|strcoll|strxfrm|strspn|strcspn|strpbrk|strtok|strtok_r|memmem|"
        "isalnum|isalpha|isblank|iscntrl|isdigit|isgraph|islower|isprint|ispunct|isspace|isupper|isxdigit|tolower|toupper|"
        "iswalpha|iswblank|iswcntrl|iswdigit|iswlower|iswprint|iswpunct|iswspace|iswupper|iswxdigit|towlower|towupper|"
        "wcslen|wmemchr|wmemcmp|wmemcpy|wmemmove|wmemset|wcscoll|wcsxfrm|wcstod|wcstof|wcstol|wcstoll|wcstoul|wcstoull|"
        "sin|cos|tan|asin|acos|atan|atan2|sinf|cosf|atan2f|sqrt|sqrtf|pow|powf|exp|expf|log|logf|log10|log10f|fmod|fmodf|"
        "fabs|fabsf|floor|floorf|ceil|ceilf|lroundf|modf|frexp|scalbn|time|localtime|localtime_r|gmtime|gmtime_r|ctime|strftime|"
        "__cxa_guard_acquire|__cxa_guard_release|__cxa_guard_abort|freelocale|duplocale|uselocale|mbtowc|btowc|wctob|";
    if (supported.find("|" + name + "|") != std::string::npos) return {dlsym(RTLD_DEFAULT, name.c_str()), false, false};
    return {};
}

void* guestThreadPointer() {
    thread_local std::array<uintptr_t, 64> slots{};
    slots[0] = reinterpret_cast<uintptr_t>(slots.data());
    slots[1] = reinterpret_cast<uintptr_t>(pthread_self());
    slots[5] = *static_cast<uintptr_t*>(bridgeSymbol("__stack_chk_guard").address);
    return slots.data();
}

void* guardedImport(const std::string& name) {
    for (const auto& guard : guards) if (guard->name == name) return guard->address;
    if (guard_finished) throw std::runtime_error("new import after guard finalization");
    if (!guard_memory) {
        guard_size = size_t(getpagesize()) * 8;
        void* memory = mmap(nullptr, guard_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (memory == MAP_FAILED) throw std::runtime_error("guard allocation failed");
        guard_memory = static_cast<uint8_t*>(memory);
    }
    if (guard_used + 32 > guard_size) throw std::runtime_error("import guard capacity exceeded");
    auto entry = std::make_unique<Guard>(); entry->name = name; entry->address = guard_memory + guard_used;
    uint32_t code[] = {0xd503245f, 0x58000060, 0x58000090, 0xd61f0200};
    std::memcpy(entry->address, code, 16);
    uintptr_t text = reinterpret_cast<uintptr_t>(entry->name.c_str()), function = reinterpret_cast<uintptr_t>(&unknownFunction);
    std::memcpy(guard_memory + guard_used + 16, &text, 8); std::memcpy(guard_memory + guard_used + 24, &function, 8);
    void* result = entry->address; guards.push_back(std::move(entry)); guard_used += 32;
    return result;
}
void finishImportGuards() {
    if (!guard_memory || guard_finished) return;
    if (mprotect(guard_memory, guard_size, PROT_READ | PROT_EXEC)) throw std::runtime_error("guard execution protection failed");
    __builtin___clear_cache(reinterpret_cast<char*>(guard_memory), reinterpret_cast<char*>(guard_memory + guard_used));
    guard_finished = true;
}
} // namespace native_poc
