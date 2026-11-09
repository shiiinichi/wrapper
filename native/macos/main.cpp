#include "elf_loader.h"
#include "jni_host.h"
#include "abi_selftest.h"
#include "services.h"
#include "authentication.h"
#include "native_api.h"

#if !defined(__APPLE__) || !defined(__aarch64__)
#error "This proof of concept must be compiled as a native arm64 macOS executable."
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

using native_poc::Loader;
using native_poc::jsonString;
using native_poc::loaderReport;

namespace {

template <typename Function> Function function(Loader& loader, const std::string& library, const char* name) {
    void* pointer = loader.symbol(library, name);
    for (const auto& image : loader.images())
        if (image->report.library == library && image->executableAddress(reinterpret_cast<uintptr_t>(pointer)))
            return reinterpret_cast<Function>(pointer);
    throw std::runtime_error(std::string("function is outside executable ELF segments: ") + name);
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string probeZlib(Loader& loader) {
    constexpr const char* library = "libz.so";
    loader.load(library);
    loader.relocate();
    loader.prepareExecution();
    loader.initialize();
    auto version = function<const char* (*)()>(loader, library, "zlibVersion");
    auto compress = function<int (*)(unsigned char*, unsigned long*, const unsigned char*, unsigned long, int)>(
        loader, library, "compress2");
    auto uncompress = function<int (*)(unsigned char*, unsigned long*, const unsigned char*, unsigned long)>(
        loader, library, "uncompress");
    auto crc = function<unsigned long (*)(unsigned long, const unsigned char*, unsigned int)>(loader, library, "crc32");
    std::string input;
    for (int i = 0; i < 128; ++i) input += "Native macOS ARM64 -> Android ELF -> memory bridge.\n";
    std::vector<unsigned char> compressed(input.size() + 1024), restored(input.size());
    unsigned long compressed_size = compressed.size(), restored_size = restored.size();
    require(compress(compressed.data(), &compressed_size,
        reinterpret_cast<const unsigned char*>(input.data()), input.size(), 9) == 0, "Android compress2 failed");
    require(compressed_size <= compressed.size(), "Android compression returned an invalid length");
    require(uncompress(restored.data(), &restored_size, compressed.data(), compressed_size) == 0,
        "Android uncompress failed");
    require(restored_size == input.size() && std::memcmp(restored.data(), input.data(), input.size()) == 0,
        "Android zlib round trip changed the payload");
    const char crc_input[] = "The quick brown fox jumps over the lazy dog";
    auto checksum = crc(0, reinterpret_cast<const unsigned char*>(crc_input), sizeof(crc_input) - 1);
    require(checksum == 0x414fa339UL, "Android crc32 returned the wrong checksum");
    return "{\"version\":" + jsonString(version()) + ",\"input_bytes\":" + std::to_string(input.size()) +
        ",\"compressed_bytes\":" + std::to_string(compressed_size) + ",\"crc32\":" +
        std::to_string(checksum) + ",\"round_trip\":true}";
}

struct BlockDescriptor {
    unsigned long reserved, size;
    void (*copy)(void*, const void*);
    void (*dispose)(const void*);
};
struct BlockLiteral {
    void* isa;
    int flags, reserved;
    int (*invoke)(void*, int);
    BlockDescriptor* descriptor;
    int captured;
};
int copy_callbacks = 0, dispose_callbacks = 0;
extern "C" int native_block_invoke(void* block, int value) {
    return static_cast<BlockLiteral*>(block)->captured + value;
}
extern "C" void native_block_copy(void* destination, const void* source) {
    ++copy_callbacks;
    static_cast<BlockLiteral*>(destination)->captured = static_cast<const BlockLiteral*>(source)->captured;
}
extern "C" void native_block_dispose(const void*) { ++dispose_callbacks; }

std::string probeBlocks(Loader& loader) {
    constexpr const char* library = "libBlocksRuntime.so";
    loader.load(library);
    loader.relocate();
    loader.prepareExecution();
    loader.initialize();
    auto copy = function<void* (*)(const void*)>(loader, library, "_Block_copy");
    auto release = function<void (*)(const void*)>(loader, library, "_Block_release");
    BlockDescriptor descriptor{0, sizeof(BlockLiteral), native_block_copy, native_block_dispose};
    BlockLiteral stack{loader.symbol(library, "_NSConcreteStackBlock"), 1 << 25, 0,
        native_block_invoke, &descriptor, 37};
    auto* heap = static_cast<BlockLiteral*>(copy(&stack));
    require(heap && heap != &stack, "Android Blocks runtime did not copy the stack block");
    require(copy_callbacks == 1, "Android ELF did not invoke the native copy callback");
    int result = heap->invoke(heap, 5);
    require(result == 42, "copied block lost its capture or invoke pointer");
    require(copy(heap) == heap, "Android Blocks runtime did not retain the heap block");
    release(heap);
    require(dispose_callbacks == 0, "Android Blocks runtime disposed a retained block early");
    release(heap);
    require(dispose_callbacks == 1, "Android ELF did not invoke the native dispose callback");
    return "{\"invoke_result\":42,\"copy_callbacks\":1,\"dispose_callbacks\":1,\"retain_release\":true}";
}

} // namespace

int main(int argc, char** argv) {
    // Each invocation is an isolated, short-lived probe; never create core dumps.
    rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    std::unique_ptr<Loader> loader;
    std::string assets, mode, library, data_directory, prompt_script, adam_id, key_uri;
    bool apple_profile = false;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if ((arg == "--assets" || arg == "--data-dir" || arg == "--prompt-script" || arg == "--adam-id" || arg == "--key-uri" || arg == "--api" || arg == "--probe" || arg == "--inspect" || arg == "--initialize") && i + 1 < argc) {
                if (arg == "--assets") assets = argv[++i];
                else if (arg == "--data-dir") data_directory = argv[++i];
                else if (arg == "--prompt-script") prompt_script = argv[++i];
                else if (arg == "--adam-id") adam_id = argv[++i];
                else if (arg == "--key-uri") key_uri = argv[++i];
                else {
                    if (!mode.empty()) throw std::runtime_error("choose exactly one probe/inspect/initialize mode");
                    mode = arg;
                    library = argv[++i];
                }
            } else if (arg == "--apple-profile") {
                apple_profile = true;
            } else if (arg == "--help") {
                std::cout << "usage: wrapper-native-poc --assets DIR [--apple-profile] [--probe NAME | --inspect LIB.so | --initialize LIB.so] [--data-dir DIR]\n"
                    "probes: zlib, blocks, coreadi-jni, lskd-jni, services, auth-preflight, auth-network, auth-status\n"
                    "interactive sign-in: ./script/build_and_run.sh --login\n"
                    "native HTTP API: ./script/build_and_run.sh --serve\n";
                return 0;
            } else throw std::runtime_error("unknown or incomplete argument: " + arg);
        }
        if (assets.empty() || mode.empty()) throw std::runtime_error("--assets and one operation are required");
        loader = std::make_unique<Loader>(assets, apple_profile);
        std::string result = "null";
        bool operation_ok = true;
        if (mode == "--api" && apple_profile) {
            result = native_poc::nativeApi(*loader, data_directory, library, adam_id, key_uri);
        } else if (mode == "--probe") {
            if (library == "zlib") result = probeZlib(*loader);
            else if (library == "blocks") result = probeBlocks(*loader);
            else if (library == "coreadi-jni" && apple_profile) result = native_poc::probeAppleJni(*loader, "libCoreADI.so");
            else if (library == "lskd-jni" && apple_profile) result = native_poc::probeAppleJni(*loader, "libCoreLSKD.so");
            else if (library == "tls-abi" && apple_profile) result = native_poc::probeTlsAbi(*loader);
            else if (library == "runtime-abi" && apple_profile) result = native_poc::probeRuntimeAbi(assets);
            else if (library == "services" && apple_profile) result = native_poc::probeServices(*loader, data_directory);
            else if (library == "service-abi" && apple_profile) result = native_poc::probeServiceAbi(*loader, assets);
            else if (library == "auth-preflight" && apple_profile) result = native_poc::authenticate(*loader, data_directory, true).evidence;
            else if (library == "auth-network" && apple_profile) result = native_poc::probeAuthNetwork(*loader, data_directory);
            else if (library == "auth-login" && apple_profile) {
                auto authentication = native_poc::authenticate(*loader, data_directory, false, prompt_script);
                result = authentication.evidence; operation_ok = authentication.authenticated;
            }
            else if (library == "auth-abi" && apple_profile) result = native_poc::probeAuthenticationAbi(*loader, data_directory);
            else if (library == "auth-status" && apple_profile) result = native_poc::authenticationStatus(*loader, data_directory);
            else if (library == "network-abi" && apple_profile) result = native_poc::probeNetworkAbi(assets);
            else if (library == "corefoundation-abi" && apple_profile) result = native_poc::probeCoreFoundationAbi(*loader);
            else throw std::runtime_error("unknown probe or required Apple profile was not enabled");
        } else {
            loader->load(library);
            loader->relocate();
            if (mode == "--initialize") { native_poc::installCrashDiagnostics(*loader); loader->prepareExecution(); loader->initialize(); }
        }
        std::cout << "{\"ok\":" << (operation_ok ? "true" : "false") << ",\"operation\":" << jsonString(mode) << ",\"target\":" << jsonString(library)
            << ",\"result\":" << result << ",\"loader\":" << loaderReport(*loader) << "}\n";
        // Services own background ELF workers. This isolated probe gives their
        // images process lifetime; do not unmap live worker instructions.
        if (mode == "--api" || (mode == "--probe" && (library == "services" || library.rfind("auth-", 0) == 0))) { std::cout.flush(); std::_Exit(operation_ok ? 0 : 1); }
        return 0;
    } catch (const std::exception& error) {
        std::cout << "{\"ok\":false,\"error\":" << jsonString(error.what()) << ",\"loader\":"
            << (loader ? loaderReport(*loader) : "null") << "}\n";
        if (mode == "--api" || (mode == "--probe" && (library == "services" || library.rfind("auth-", 0) == 0))) { std::cout.flush(); std::_Exit(1); }
        return 1;
    }
}
