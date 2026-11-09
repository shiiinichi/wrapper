#include "abi_selftest.h"
#include "abi_runtime.h"
#include "elf_loader.h"
#include "guest_call.h"
#include "posix_bridge.h"
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <pthread.h>

namespace native_poc {
namespace {
void check(bool result, const char* text) { if (!result) throw std::runtime_error(text); }
template <typename Function> Function bridge(const char* name) {
    auto result = runtimeBridge(name);
    if (!result.address || result.guarded || result.data) throw std::runtime_error("ABI test bridge is unavailable");
    return reinterpret_cast<Function>(result.address);
}
std::atomic<unsigned> once_calls{0};
void onceCallback() { ++once_calls; }
struct OnceTest { void* key; int (*once)(void*, void (*)()); int result = -1; };
void* onceThread(void* pointer) {
    auto* test = static_cast<OnceTest*>(pointer); test->result = test->once(test->key, onceCallback); return pointer;
}
}

std::string probeTlsAbi(Loader& loader) {
    loader.load("tls-fixture.so"); loader.relocate(); loader.prepareExecution(); loader.initialize();
    auto function = reinterpret_cast<uintptr_t (*)()>(loader.symbol("tls-fixture.so", "tls_probe"));
    std::array<uintptr_t, 3> pointers{};
    pointers[0] = function();
    std::atomic<unsigned> ready{0}; std::atomic<bool> release{false};
    auto call = [&](size_t index) {
        pointers[index] = function(); ++ready;
        while (!release.load()) std::this_thread::yield();
    };
    std::thread first(call, 1), second(call, 2);
    while (ready.load() != 2) std::this_thread::yield();
    release.store(true); first.join(); second.join();
    check(pointers[0] && pointers[1] && pointers[2], "native TLS fixture lost registers, flags, or vector state");
    check(pointers[0] != pointers[1] && pointers[1] != pointers[2] && pointers[0] != pointers[2], "guest TLS is not per-thread");
    return "{\"state_preserved\":true,\"independent_threads\":3}";
}

std::string probeRuntimeAbi(const std::string& assets) {
    alignas(8) std::array<unsigned char, 40> mutex{};
    alignas(8) std::array<unsigned char, 48> condition{};
    auto lock = bridge<int (*)(void*)>("pthread_mutex_lock");
    auto unlock = bridge<int (*)(void*)>("pthread_mutex_unlock");
    auto destroy = bridge<int (*)(void*)>("pthread_mutex_destroy");
    auto wait = bridge<int (*)(void*, void*)>("pthread_cond_wait");
    auto signal = bridge<int (*)(void*)>("pthread_cond_signal");
    auto destroy_condition = bridge<int (*)(void*)>("pthread_cond_destroy");
    auto create_key = bridge<int (*)(uint32_t*, void (*)(void*))>("pthread_key_create");
    auto set_key = bridge<int (*)(uint32_t, const void*)>("pthread_setspecific");
    auto get_key = bridge<void* (*)(uint32_t)>("pthread_getspecific");
    auto delete_key = bridge<int (*)(uint32_t)>("pthread_key_delete");
    uint32_t key = 0; check(create_key(&key, nullptr) == 0, "guest key creation failed");
    int main_value = 11, thread_value = 22;
    check(set_key(key, &main_value) == 0, "main-thread key write failed");
    bool waiting = false, proceed = false;
    std::atomic<bool> thread_ok{false};
    std::thread thread([&] {
        bool ok = get_key(key) == nullptr && set_key(key, &thread_value) == 0 && get_key(key) == &thread_value;
        ok = lock(mutex.data()) == 0 && ok; waiting = true;
        while (!proceed) ok = wait(condition.data(), mutex.data()) == 0 && ok;
        ok = unlock(mutex.data()) == 0 && ok; thread_ok.store(ok);
    });
    for (;;) {
        check(lock(mutex.data()) == 0, "guest mutex lock failed");
        if (waiting) { proceed = true; check(signal(condition.data()) == 0, "guest condition signal failed"); check(unlock(mutex.data()) == 0, "guest mutex unlock failed"); break; }
        check(unlock(mutex.data()) == 0, "guest mutex unlock failed"); std::this_thread::yield();
    }
    thread.join();
    check(thread_ok.load() && get_key(key) == &main_value, "guest pthread state was not isolated");
    check(delete_key(key) == 0, "guest key deletion failed");
    check(lock(mutex.data()) == 0 && destroy(mutex.data()) != 0, "busy guest mutex was incorrectly destroyed");
    check(unlock(mutex.data()) == 0 && destroy(mutex.data()) == 0 && destroy_condition(condition.data()) == 0,
        "guest pthread cleanup failed");
    auto syscall = bridge<long (*)(long, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t)>("syscall");
    auto get_errno = bridge<int* (*)()>("__errno");
    auto open_file = bridge<void* (*)(const char*, const char*)>("fopen");
    auto read_file = bridge<size_t (*)(void*, size_t, size_t, void*)>("fread");
    auto close_file = bridge<int (*)(void*)>("fclose");
    void* input = open_file((assets + "/abi-read.txt").c_str(), "rb");
    check(input != nullptr, "guest FILE opening failed");
    char contents[64]{};
    size_t bytes = read_file(contents, 1, sizeof(contents), input);
    constexpr char expected[] = "typed-android-file-bridge\n";
    check(bytes == sizeof(expected) - 1 && std::memcmp(contents, expected, bytes) == 0, "guest FILE contents changed");
    check(close_file(input) == 0, "guest FILE closing failed");
    check(open_file((assets + "/does-not-exist").c_str(), "rb") == nullptr && *get_errno() == 2,
        "guest FILE errors did not set Android errno");
    int value = 0;
    check(syscall(98, reinterpret_cast<uintptr_t>(&value), 128, 1, 0, 0, 0) == -1 && *get_errno() == 11,
        "futex WAIT did not return Linux EAGAIN for a changed value");
    std::atomic<bool> waiter_ok{false};
    std::thread futex_thread([&] {
        int64_t timeout[] = {2, 0};
        waiter_ok.store(syscall(98, reinterpret_cast<uintptr_t>(&value), 128, 0, reinterpret_cast<uintptr_t>(timeout), 0, 0) == 0);
    });
    long woke = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!woke && std::chrono::steady_clock::now() < deadline) {
        woke = syscall(98, reinterpret_cast<uintptr_t>(&value), 129, 1, 0, 0, 0);
        std::this_thread::yield();
    }
    futex_thread.join();
    check(woke == 1 && waiter_ok.load(), "futex WAIT/WAKE did not synchronize a real waiter");
    alignas(8) std::array<unsigned char, 64> rwlock{};
    auto read_lock = bridge<int (*)(void*)>("pthread_rwlock_rdlock");
    auto write_lock = bridge<int (*)(void*)>("pthread_rwlock_wrlock");
    auto unlock_rw = bridge<int (*)(void*)>("pthread_rwlock_unlock");
    auto destroy_rw = bridge<int (*)(void*)>("pthread_rwlock_destroy");
    check(read_lock(rwlock.data()) == 0 && unlock_rw(rwlock.data()) == 0 && write_lock(rwlock.data()) == 0 &&
        unlock_rw(rwlock.data()) == 0 && destroy_rw(rwlock.data()) == 0, "guest rwlock synchronization failed");
    uint32_t once_state = 0;
    auto once = bridge<int (*)(void*, void (*)())>("pthread_once");
    auto create_thread = bridge<int (*)(pthread_t*, const void*, void* (*)(void*), void*)>("pthread_create");
    auto join_thread = bridge<int (*)(pthread_t, void**)>("pthread_join");
    std::array<OnceTest, 2> tests{{{&once_state, once}, {&once_state, once}}};
    std::array<pthread_t, 2> threads{};
    for (size_t i = 0; i < threads.size(); ++i) check(create_thread(&threads[i], nullptr, onceThread, &tests[i]) == 0, "guest pthread_create failed");
    for (size_t i = 0; i < threads.size(); ++i) {
        void* result = nullptr;
        check(join_thread(threads[i], &result) == 0 && result == &tests[i] && tests[i].result == 0, "guest pthread_join failed");
    }
    check(once(&once_state, onceCallback) == 0 && once_calls == 1, "guest pthread_once repeated its callback");
    return "{\"guest_mutex_condition\":true,\"guest_keys_isolated\":true,\"futex_wait_wake\":true,\"linux_errno\":true,\"guest_stdio\":true,\"rwlocks_threads_once\":true}";
}

std::string probeServiceAbi(Loader& loader, const std::string& assets) {
    loader.load("tls-fixture.so"); loader.relocate(); loader.prepareExecution(); loader.initialize();
    auto* sret = loader.symbol("tls-fixture.so", "sret_probe");
    std::array<uintptr_t, 6> output{};
    callGuestReturn(sret, output.data(), reinterpret_cast<void*>(0x11), reinterpret_cast<void*>(0x22),
        reinterpret_cast<void*>(0x33), reinterpret_cast<void*>(0x44), reinterpret_cast<void*>(0x55), reinterpret_cast<void*>(0x66));
    check(output == std::array<uintptr_t, 6>{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, "ARM64 indirect return moved named arguments");
    std::array<char, 256> text{};
    auto formatter = reinterpret_cast<int (*)(void*, char*)>(loader.symbol("tls-fixture.so", "format_probe"));
    int length = formatter(runtimeBridge("snprintf").address, text.data());
    constexpr char expected[] = "7 -9 1234 abc B 3.25 305419896    abc 0.5 0.5 0.5 0.5 0.5 0.5 0.5 3.5";
    check(length == sizeof(expected) - 1 && std::strcmp(text.data(), expected) == 0, "Android variadic register/stack formatting failed");
    auto open_image = bridge<void* (*)(const char*, int)>("dlopen");
    auto find_symbol = bridge<void* (*)(void*, const char*)>("dlsym");
    auto close_image = bridge<int (*)(void*)>("dlclose");
    auto* handle = open_image("/guest/lib64/tls-fixture.so", 2);
    check(handle && find_symbol(handle, "sret_probe") == sret && close_image(handle) == 0, "guest dynamic lookup did not use loaded ELF symbols");

    auto directory = assets + "/writes";
    std::filesystem::create_directory(directory); setRuntimeDirectory(directory);
    auto open_file = bridge<int (*)(const char*, int, unsigned)>("open");
    auto close_file = bridge<int (*)(int)>("close");
    auto write_file = bridge<ssize_t (*)(int, const void*, size_t)>("write");
    auto read_file = bridge<ssize_t (*)(int, void*, size_t, int64_t)>("pread");
    auto stat_file = bridge<int (*)(int, void*)>("fstat");
    int fd = open_file((directory + "/abi-file").c_str(), 2 | 0100 | 0200, 0600);
    check(fd >= 0, "Android open flags were not translated");
    constexpr char payload[] = "native-bootstrap-file";
    check(write_file(fd, payload, sizeof(payload) - 1) == sizeof(payload) - 1, "Android file write failed");
    std::array<unsigned char, 144> status; status.fill(0xa5);
    check(stat_file(fd, status.data()) == 0, "Android stat failed");
    int64_t bytes = 0; std::memcpy(&bytes, status.data() + 48, 8);
    check(bytes == sizeof(payload) - 1 && status[128] == 0xa5 && status[143] == 0xa5, "Android stat layout overwrote caller storage");
    std::array<char, 64> contents{};
    check(read_file(fd, contents.data(), contents.size(), 0) == sizeof(payload) - 1 && std::strcmp(contents.data(), payload) == 0, "Android pread changed file bytes");
    check(close_file(fd) == 0, "Android close failed");
    check(open_file((directory + "/abi-file").c_str(), 2 | 0100 | 0200, 0600) == -1 && guestErrno() == 17, "Android O_EXCL did not preserve existing files");
    check(open_file((assets + "/outside-write").c_str(), 1 | 0100, 0600) == -1 && guestErrno() == 13, "guest file writes escaped the dedicated directory");
    std::filesystem::create_directory(assets + "/outside");
    std::filesystem::create_directory_symlink(assets + "/outside", directory + "/escape");
    check(open_file((directory + "/escape/forbidden").c_str(), 1 | 0100, 0600) == -1 && guestErrno() == 13, "guest writes followed a symlink outside the dedicated directory");
    auto get_time = bridge<int (*)(void*, void*)>("gettimeofday");
    std::array<int64_t, 3> time{0, 0, 0x123456789};
    check(get_time(time.data(), nullptr) == 0 && time[0] > 0 && time[1] >= 0 && time[1] < 1000000 && time[2] == 0x123456789, "Android timeval layout changed");
    auto config = bridge<long (*)(int)>("sysconf");
    check(config(0x27) == getpagesize() && config(0x7fffffff) == -1 && guestErrno() == 22, "Android sysconf names were not translated");
    auto sha1 = bridge<unsigned char* (*)(const void*, size_t, unsigned char*)>("SHA1");
    std::array<unsigned char, 20> hash{};
    const unsigned char digest[] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    check(sha1("abc", 3, hash.data()) == hash.data() && std::memcmp(hash.data(), digest, 20) == 0, "SHA1 adapter changed the standard digest");
    return "{\"indirect_return\":true,\"guest_variadic_format\":true,\"elf_dynamic_lookup\":true,\"android_file_stat\":true,"
        "\"isolated_writes\":true,\"time_sysconf\":true,\"sha1\":true}";
}
}
