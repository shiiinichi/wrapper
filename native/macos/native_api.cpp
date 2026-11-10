#include "native_api.h"
#include "auth_input.h"
#include "elf_loader.h"
#include "format_bridge.h"
#include "guest_call.h"
#include "services.h"
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <regex>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include "storefront_ids.inc"
#include "fairplay_certificate.h"

namespace native_poc {
namespace {
constexpr const char* music = "libandroidappmusic.so";
constexpr const char* store = "libstoreservicescore.so";
constexpr const char* platform = "libmediaplatform.so";
constexpr const char* strings = "libc++_shared.so";
template<class Function> Function fn(Loader& loader, const char* library, const char* symbol) {
    auto address = loader.symbol(library, symbol);
    for (const auto& image : loader.images())
        if (image->report.library == library && image->executableAddress(reinterpret_cast<uintptr_t>(address))) return reinterpret_cast<Function>(address);
    throw std::runtime_error(std::string("native API function is not executable: ") + symbol);
}
std::string text(const void* object) {
    if (!object) return "";
    auto bytes = static_cast<const unsigned char*>(object);
    size_t size = bytes[0] >> 1; const char* data = reinterpret_cast<const char*>(bytes + 1);
    if (bytes[0] & 1) { std::memcpy(&size, bytes + 8, 8); std::memcpy(&data, bytes + 16, 8); }
    else if (size > 22) throw std::runtime_error("invalid guest short string");
    if (size > 8 * 1024 * 1024 || (!data && size)) throw std::runtime_error("invalid guest string length");
    return std::string(data, size);
}
struct OwnedString {
    Loader& loader;
    alignas(8) std::array<unsigned char, 24> bytes{};
    explicit OwnedString(Loader& owner) : loader(owner) {}
    OwnedString(Loader& owner, const std::string& value) : loader(owner) {
        fn<void (*)(void*, const char*, size_t)>(loader, strings, "_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE6__initEPKcm")(bytes.data(), value.data(), value.size());
    }
    ~OwnedString() { fn<void (*)(void*)>(loader, strings, "_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev")(bytes.data()); }
    std::string value() const { return text(bytes.data()); }
};
void error(Loader& loader, const GuestShared* condition, const char* operation) {
    if (!condition || !condition->object) return;
    auto code = fn<const int* (*)(void*)>(loader, store, "_ZNK17storeservicescore19StoreErrorCondition9errorCodeEv")(condition->object);
    throw std::runtime_error(std::string(operation) + " failed (store code " + std::to_string(code ? *code : -1) + ")");
}
std::string storefront(Loader& loader, const Services& services) {
    GuestShared bag; OwnedString value(loader);
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore14RequestContext20storeFrontIdentifierERKNSt6__ndk110shared_ptrINS_6URLBagEEE"), value.bytes.data(), services.context.object, &bag);
    auto raw = value.value();
    return raw.size() == 2 ? raw : builtin_storefront_region(raw.substr(0, raw.find('-')));
}
bool accountPresent(Loader& loader, const Services& services) {
    GuestShared account;
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore14RequestContext7accountEv"), &account, services.context.object);
    return account.object && account.control;
}
std::string signedRequest(Loader& loader, const Services& services, const std::string& url, const std::string& body, bool developer) {
    auto allocation = static_cast<unsigned char*>(std::calloc(1, 512));
    if (!allocation) throw std::runtime_error("HTTPMessage allocation failed");
    auto table = static_cast<unsigned char*>(loader.symbol(music, "_ZTVNSt6__ndk120__shared_ptr_emplaceIN13mediaplatform11HTTPMessageENS_9allocatorIS2_EEEE"));
    *reinterpret_cast<void**>(allocation) = table + 16;
    GuestShared message{allocation + 32, allocation};
    OwnedString address(loader, url), method(loader, body.empty() ? "GET" : "POST");
    fn<void (*)(void*, void*, void*)>(loader, platform, "_ZN13mediaplatform11HTTPMessageC2ENSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES7_")(message.object, address.bytes.data(), method.bytes.data());
    auto header = [&](const char* name, const char* value) {
        GuestString key(name), content(value);
        fn<void (*)(void*, const GuestString*, const GuestString*)>(loader, platform, "_ZN13mediaplatform11HTTPMessage9setHeaderERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_")(message.object, &key, &content);
    };
    if (!body.empty()) {
        header("Content-Type", "application/json; charset=UTF-8"); header("Expect", "");
        header("X-Apple-Requesting-Bundle-Id", "com.apple.android.music");
        header("X-Apple-Requesting-Bundle-Version", "Music/4.9 Android/10 model/Samsung S9 build/7663313 (dt:66)");
        fn<void (*)(void*, char*, size_t)>(loader, platform, "_ZN13mediaplatform11HTTPMessage11setBodyDataEPcm")(message.object, const_cast<char*>(body.data()), body.size());
    }
    GuestShared request;
    callGuestReturn(loader.symbol(music, "_ZNSt6__ndk110shared_ptrIN17storeservicescore10URLRequestEE11make_sharedIJRNS0_IN13mediaplatform11HTTPMessageEEERKNS0_INS1_14RequestContextEEEEEES3_DpOT_"), &request, &message, &services.context);
    if (!request.object) throw std::runtime_error("URLRequest factory returned empty");
    if (developer) {
        auto parameter = [&](const char* name, const char* value) { GuestString key(name), content(value);
            fn<void (*)(void*, const GuestString*, const GuestString*)>(loader, store, "_ZN17storeservicescore10URLRequest19setRequestParameterERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_")(request.object, &key, &content); };
        parameter("clientId", "musicAndroid"); parameter("version", "1");
    }
    fn<void (*)(void*, double)>(loader, store, "_ZN17storeservicescore10URLRequest14runWithTimeoutENSt6__ndk16chrono8durationIdNS1_5ratioILl1ELl1EEEEE")(request.object, 25.0);
    error(loader, fn<const GuestShared* (*)(void*)>(loader, store, "_ZNK17storeservicescore10URLRequest5errorEv")(request.object), "signed URL request");
    auto response = fn<const GuestShared* (*)(void*)>(loader, store, "_ZNK17storeservicescore10URLRequest8responseEv")(request.object);
    if (!response || !response->object) throw std::runtime_error("signed URL request returned no response");
    auto underlying = fn<const GuestShared* (*)(void*)>(loader, store, "_ZNK17storeservicescore11URLResponse18underlyingResponseEv")(response->object);
    if (!underlying || !underlying->object) throw std::runtime_error("URL response has no HTTP message");
    void* data = nullptr; std::memcpy(&data, static_cast<unsigned char*>(underlying->object) + 48, 8);
    if (!data) throw std::runtime_error("HTTP response has no body data");
    auto bytes = fn<const char* (*)(void*)>(loader, platform, "_ZNK13mediaplatform4Data5bytesEv")(data);
    auto size = fn<size_t (*)(void*)>(loader, platform, "_ZNK13mediaplatform4Data6lengthEv")(data);
    if (!bytes || !size || size > 1024 * 1024) throw std::runtime_error("HTTP response body length is invalid");
    return std::string(bytes, size);
}
std::string field(const std::string& json, const std::string& name) {
    std::smatch match;
    if (!std::regex_search(json, match, std::regex("\"" + name + "\"\\s*:\\s*\"([^\"\\\\]+)\""))) throw std::runtime_error("token response is missing a required field");
    return match[1];
}
std::string tokens(Loader& loader, const Services& services) {
    auto developer = field(signedRequest(loader, services, "https://sf-api-token-service.itunes.apple.com/apiToken", "", true), "token");
    GuestShared guid, instance;
    callGuestReturn(loader.symbol(store, "_ZN17storeservicescore10DeviceGUID8instanceEv"), &instance);
    callGuestReturn(loader.symbol(store, "_ZN17storeservicescore10DeviceGUID4guidEv"), &guid, instance.object);
    if (!guid.object) throw std::runtime_error("DeviceGUID has no data");
    auto bytes = fn<const char* (*)(void*)>(loader, platform, "_ZNK13mediaplatform4Data5bytesEv")(guid.object);
    auto size = fn<size_t (*)(void*)>(loader, platform, "_ZNK13mediaplatform4Data6lengthEv")(guid.object);
    if (!bytes || !size || size > 1024) throw std::runtime_error("DeviceGUID length is invalid");
    std::string identifier(bytes, strnlen(bytes, size));
    auto body = "{\"guid\":" + jsonString(identifier) + ",\"assertion\":" + jsonString(developer) + ",\"tcc-acceptance-date\":\"" + std::to_string(static_cast<long long>(time(nullptr)) * 1000) + "\"}";
    auto user = field(signedRequest(loader, services, "https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/createMusicToken", body, false), "music_token");
    return "{\"storefront\":" + jsonString(storefront(loader, services)) + ",\"developer_token\":" + jsonString(developer) + ",\"music_token\":" + jsonString(user) + "}";
}
}
std::string nativeApi(Loader& loader, const std::string& directory, const std::string& operation, const std::string& adam, const std::string& uri) {
    if (operation != "status" && operation != "tokens" && operation != "key") throw std::runtime_error("unknown native API operation");
    if (operation == "key" && (adam.empty() || adam.size() > 19 || !std::all_of(adam.begin(), adam.end(), [](char c) { return c >= '0' && c <= '9'; }))) throw std::runtime_error("invalid adamId");
    if (operation == "key" && (uri.rfind("skd://", 0) != 0 || uri.size() > 8192 || (adam != "0" && uri == "skd://itunes.apple.com/P000000000/s1/e1"))) throw std::runtime_error("invalid key URI");
    if (operation == "tokens" && isatty(STDOUT_FILENO)) throw std::runtime_error("token exchange is an internal pipe operation");
    setGuestLogging(false);
    auto services = initializeServices(loader, directory, profileIdentity(directory));
    bool account = accountPresent(loader, services);
    if (operation == "status") return "{\"regions\":[" + (account ? jsonString(storefront(loader, services)) : "") + "],\"account_present\":" + (account ? "true" : "false") + ",\"backend\":\"macos-arm64\",\"temari_compatible\":false,\"native_sample_decryption\":true}";
    if (!account) throw std::runtime_error("no saved account; sign in first");
    if (operation == "tokens") return tokens(loader, services);
    GuestString identifier(adam.c_str()), key_uri(uri.c_str()), format("com.apple.streamingkeydelivery"), version("1"), server("https://play.itunes.apple.com/WebObjects/MZPlay.woa/music/fps"), protocol("simplified"), certificate(fairplayCertificate);
    const void* arguments[] = {services.foothill, &identifier, &identifier, &key_uri, &format, &version, &server, &protocol, &certificate};
    GuestShared key;
    callGuestReturn9(loader.symbol(music, "_ZN21SVFootHillSessionCtrl16getPersistentKeyERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_S8_S8_S8_S8_S8_S8_"), &key, arguments);
    auto content = text(key.object);
    if (content.empty()) throw std::runtime_error("native FairPlay key retrieval returned empty");
    return "{\"adamId\":" + jsonString(adam) + ",\"keyUri\":" + jsonString(uri) + ",\"contentKey\":" + jsonString(content) + ",\"temariCompatible\":false}";
}

namespace {
using Bytes = std::vector<unsigned char>;
constexpr size_t maximumFrame = 16 * 1024 * 1024;
void appendWord(Bytes& bytes, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<unsigned char>(value >> shift));
}
class Frame {
public:
    explicit Frame(const Bytes& value) : bytes(value) {}
    uint32_t word() {
        if (bytes.size() - offset < 4) throw std::runtime_error("truncated sample frame");
        uint32_t value = 0;
        for (unsigned n = 0; n < 4; ++n) value = (value << 8) | bytes[offset++];
        return value;
    }
    Bytes take(size_t count) {
        if (count > bytes.size() - offset) throw std::runtime_error("truncated sample frame");
        Bytes value(bytes.begin() + offset, bytes.begin() + offset + count);
        offset += count;
        return value;
    }
    std::string string(size_t limit) {
        auto count = word();
        if (!count || count > limit) throw std::runtime_error("invalid sample frame string");
        auto value = take(count);
        if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }))
            throw std::runtime_error("invalid sample frame string");
        return std::string(value.begin(), value.end());
    }
    void end() const { if (offset != bytes.size()) throw std::runtime_error("trailing sample frame bytes"); }
private:
    const Bytes& bytes;
    size_t offset = 0;
};
bool readFrame(Bytes& result) {
    unsigned char header[4];
    size_t count = std::fread(header, 1, 4, stdin);
    if (!count && std::feof(stdin)) return false;
    if (count != 4) throw std::runtime_error("truncated worker header");
    uint32_t size = (uint32_t(header[0]) << 24) | (uint32_t(header[1]) << 16) | (uint32_t(header[2]) << 8) | header[3];
    if (!size || size > maximumFrame) throw std::runtime_error("invalid worker frame length");
    result.resize(size);
    if (std::fread(result.data(), 1, size, stdin) != size) throw std::runtime_error("truncated worker frame");
    return true;
}
void writeFrame(bool success, const Bytes& result) {
    Bytes header;
    appendWord(header, static_cast<uint32_t>(result.size() + 1));
    header.push_back(success ? 0 : 1);
    if (std::fwrite(header.data(), 1, header.size(), stdout) != header.size() ||
        std::fwrite(result.data(), 1, result.size(), stdout) != result.size() || std::fflush(stdout))
        throw std::runtime_error("worker response pipe closed");
}
struct NativeContext {
    Loader& loader;
    GuestShared key, owner;
    void* kd = nullptr;
    std::string content;
    explicit NativeContext(Loader& value) : loader(value) {}
    ~NativeContext() {
        for (auto* value : {&owner, &key}) if (value->control)
            fn<void (*)(void*)>(loader, strings, "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(value->control);
    }
};
std::unique_ptr<NativeContext> createContext(Loader& loader, const Services& services, const std::string& adam, const std::string& uri) {
    if (adam.empty() || adam.size() > 19 || !std::all_of(adam.begin(), adam.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
        uri.rfind("skd://", 0) != 0 || uri.size() > 8192 || (adam != "0" && uri == "skd://itunes.apple.com/P000000000/s1/e1"))
        throw std::runtime_error("invalid native context request");
    auto value = std::make_unique<NativeContext>(loader);
    GuestString identifier(adam.c_str()), keyUri(uri.c_str()), format("com.apple.streamingkeydelivery"), version("1"),
        server("https://play.itunes.apple.com/WebObjects/MZPlay.woa/music/fps"), protocol("simplified"), certificate(fairplayCertificate);
    const void* arguments[] = {services.foothill, &identifier, &identifier, &keyUri, &format, &version, &server, &protocol, &certificate};
    callGuestReturn9(loader.symbol(music, "_ZN21SVFootHillSessionCtrl16getPersistentKeyERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_S8_S8_S8_S8_S8_S8_"), &value->key, arguments);
    value->content = text(value->key.object);
    if (value->content.empty()) throw std::runtime_error("native key retrieval failed");
    uint32_t type = 5;
    bool video = false;
    callGuestReturn(loader.symbol(music, "_ZN21SVFootHillSessionCtrl14decryptContextERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERKN11SVDecryptor15SVDecryptorTypeERKb"), &value->owner,
                    services.foothill, value->key.object, &type, &video);
    if (!value->owner.object) throw std::runtime_error("native decryption context unavailable");
    auto address = fn<void** (*)(void*)>(loader, music, "_ZNK18SVFootHillPContext9kdContextEv")(value->owner.object);
    if (address && *address) std::memcpy(&value->kd, *address, sizeof(void*));
    if (!value->kd) throw std::runtime_error("native decryption context unavailable");
    return value;
}
}

void nativeDecryptionWorker(Loader& loader, const std::string& directory) {
    if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) throw std::runtime_error("sample worker requires private pipes");
    setGuestLogging(false);
    auto services = initializeServices(loader, directory, profileIdentity(directory));
    if (!accountPresent(loader, services)) throw std::runtime_error("no saved account");
    std::vector<std::unique_ptr<NativeContext>> contexts;
    writeFrame(true, Bytes{'N', 'A', 'D', '2'});
    Bytes request;
    while (readFrame(request)) {
        try {
            Frame input(request);
            auto operation = input.word();
            Bytes response;
            if (operation == 1) {
                auto adam = input.string(19), uri = input.string(8192);
                input.end();
                if (contexts.size() >= 64) throw std::runtime_error("native context capacity reached");
                auto value = createContext(loader, services, adam, uri);
                appendWord(response, static_cast<uint32_t>(contexts.size() + 1));
                appendWord(response, static_cast<uint32_t>(value->content.size()));
                response.insert(response.end(), value->content.begin(), value->content.end());
                contexts.push_back(std::move(value));
            } else if (operation == 2) {
                auto handle = input.word(), count = input.word();
                if (!handle || handle > contexts.size() || !count || count > 8192) throw std::runtime_error("invalid native sample batch");
                std::vector<Bytes> samples;
                samples.reserve(count);
                for (uint32_t n = 0; n < count; ++n) {
                    auto size = input.word();
                    if (!size || size > 4 * 1024 * 1024) throw std::runtime_error("invalid native sample size");
                    samples.push_back(input.take(size));
                }
                input.end(); // Validate the entire batch before invoking the library.
                appendWord(response, count);
                auto decrypt = fn<int (*)(void*, uint32_t, void*, void*, uint32_t)>(loader, music, "NfcRKVnxuKZy04KWbdFu71Ou");
                for (auto& sample : samples) {
                    // Temari's contract: independent samples, aligned prefix, unchanged tail.
                    auto aligned = static_cast<uint32_t>(sample.size() / 16 * 16);
                    if (aligned && decrypt(contexts[handle - 1]->kd, 5, sample.data(), sample.data(), aligned) != 0)
                        throw std::runtime_error("native sample decryption failed");
                    appendWord(response, static_cast<uint32_t>(sample.size()));
                    response.insert(response.end(), sample.begin(), sample.end());
                }
            } else if (operation == 3) {
                input.end();
                // FootHill caches persistent keys by asset, ignoring the URI on
                // cache hits. Clear that cache before opening another key URI.
                // Existing contexts remain owned by the worker's shared_ptrs.
                fn<void (*)(void*)>(loader, music, "_ZN21SVFootHillSessionCtrl16resetAllContextsEv")(services.foothill);
            } else throw std::runtime_error("unknown native sample operation");
            writeFrame(true, response);
        } catch (const std::exception&) {
            // No keys, input samples, account identifiers, or guest errors enter diagnostics.
            writeFrame(false, {});
        }
    }
}
}
