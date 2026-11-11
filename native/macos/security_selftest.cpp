#include "security_selftest.h"
#include "elf_loader.h"
#include "shims.h"
#include "abi_runtime.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <zlib.h>
#include <CommonCrypto/CommonDigest.h>

namespace native_poc {
std::string probeSecurityAbi(Loader& loader) {
    auto check = [](bool value, const char* error) { if (!value) throw std::runtime_error(error); };
    for (auto name : {"libz.so", "libxml2.so", "libicuuc_sv_apple.so"}) loader.loadHostLibrary(name);
    loader.relocate(); loader.prepareExecution(); loader.initialize();
    // Exercise the precise CVE-2022-37434 boundary against the library used by
    // guest requests, including split gzip extra-header data and a short buffer.
    auto init = reinterpret_cast<int (*)(z_streamp, int, const char*, int)>(loader.symbol("libz.so", "inflateInit2_"));
    auto get_header = reinterpret_cast<int (*)(z_streamp, gz_headerp)>(loader.symbol("libz.so", "inflateGetHeader"));
    auto inflate = reinterpret_cast<int (*)(z_streamp, int)>(loader.symbol("libz.so", "inflate"));
    auto end = reinterpret_cast<int (*)(z_streamp)>(loader.symbol("libz.so", "inflateEnd"));
    auto version = reinterpret_cast<const char* (*)()>(loader.symbol("libz.so", "zlibVersion"))();
    std::vector<unsigned char> input{0x1f,0x8b,8,4,0,0,0,0,0,3,0,2};
    input.insert(input.end(), 512, 0x42);
    input.insert(input.end(), {3,0,0,0,0,0,0,0,0,0});
    std::array<unsigned char, 48> extra{}; extra.fill(0xa5);
    std::array<unsigned char, 32> output{};
    gz_header header{}; header.extra = extra.data() + 16; header.extra_max = 16;
    z_stream stream{};
    check(init(&stream, 47, version, sizeof(stream)) == 0 && get_header(&stream, &header) == 0, "native gzip setup failed");
    stream.next_in = input.data(); stream.avail_in = 100;
    stream.next_out = output.data(); stream.avail_out = output.size();
    check(inflate(&stream, Z_NO_FLUSH) == Z_OK, "native gzip first chunk failed");
    stream.next_in = input.data() + 100; stream.avail_in = input.size() - 100;
    check(inflate(&stream, Z_NO_FLUSH) == Z_STREAM_END && header.done, "native gzip split header failed");
    end(&stream);
    for (size_t i = 0; i < extra.size(); ++i)
        check(extra[i] == (i >= 16 && i < 32 ? 0x42 : 0xa5), "native gzip extra header exceeded capacity");

    auto create = reinterpret_cast<void* (*)()>(loader.symbol("libxml2.so", "xmlDictCreate"));
    auto lookup = reinterpret_cast<const unsigned char* (*)(void*, const unsigned char*, int)>(loader.symbol("libxml2.so", "xmlDictLookup"));
    auto free = reinterpret_cast<void (*)(void*)>(loader.symbol("libxml2.so", "xmlDictFree"));
    auto dict = create(); check(dict != nullptr, "native XML dictionary allocation failed");
    unsigned char a[] = {'a'}, b[] = {'b'}, empty[] = {0};
    auto first = lookup(dict, a, 0), second = lookup(dict, b, 0), third = lookup(dict, empty, 0);
    bool same = first && first == second && second == third;
    free(dict);
    check(same, "native XML dictionary is missing the CVE-2023-29469 fix");
    unsigned char icu[4]{};
    reinterpret_cast<void (*)(unsigned char*)>(loader.symbol("libicuuc_sv_apple.so", "u_getVersion"))(icu);
    check(icu[0] >= 67, "native ICU is too old");
    auto canary = *static_cast<uint64_t*>(bridgeSymbol("__stack_chk_guard").address);
    check(canary && canary != 0x7250d94a3c81e60bULL && !(canary & 0xff), "guest stack canary is not randomized");
    check(static_cast<uintptr_t*>(guestThreadPointer())[5] == canary, "guest TLS and global stack canaries differ");
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(&canary, sizeof(canary), digest);
    std::string fingerprint;
    for (auto byte : digest) { fingerprint += "0123456789abcdef"[byte >> 4]; fingerprint += "0123456789abcdef"[byte & 15]; }
    return "{\"gzip_header_bounds\":true,\"xml_empty_dictionary\":true,\"icu_version\":" +
        jsonString(std::to_string(icu[0]) + "." + std::to_string(icu[1])) +
        ",\"stack_canary_matches_tls\":true,\"stack_canary_fingerprint\":" + jsonString(fingerprint) + "}";
}
}
