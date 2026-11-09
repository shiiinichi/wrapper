#include "abi_selftest.h"
#include "abi_runtime.h"
#include "elf_loader.h"
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <cstring>

namespace native_poc {
namespace {
void check(bool result, const char* text) { if (!result) throw std::runtime_error(text); }
template <typename Function> Function bridge(const char* name) {
    auto result = runtimeBridge(name);
    if (!result.address || result.guarded || result.data) throw std::runtime_error("ABI test bridge is unavailable");
    return reinterpret_cast<Function>(result.address);
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
    return "{\"guest_mutex_condition\":true,\"guest_keys_isolated\":true,\"futex_wait_wake\":true,\"linux_errno\":true,\"guest_stdio\":true}";
}
}
