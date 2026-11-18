#include "native_api.h"
#include "worker_protocol.h"
#include "user_profile.h"
#include "device_asset.h"
#include "music_client.h"
#include "auth_input.h"
#include "elf_loader.h"
#include "format_bridge.h"
#include "guest_call.h"
#include "services.h"
#include <array>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
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
std::string text(const void* object, size_t limit = 8 * 1024 * 1024) {
    if (!object) return "";
    auto bytes = static_cast<const unsigned char*>(object);
    size_t size = bytes[0] >> 1; const char* data = reinterpret_cast<const char*>(bytes + 1);
    if (bytes[0] & 1) { std::memcpy(&size, bytes + 8, 8); std::memcpy(&data, bytes + 16, 8); }
    else if (size > 22) throw std::runtime_error("invalid guest short string");
    if (size > limit || (!data && size)) throw std::runtime_error("invalid guest string length");
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
// These SDK getters return owning NDK vectors. Release their shared elements
// and allocation in the same guest runtime when a reusable worker finishes a request.
struct AssetItems {
    GuestShared *begin = nullptr, *end = nullptr, *capacity = nullptr;
};
struct OwnedAssetItems {
    Loader& loader;
    AssetItems items;
    explicit OwnedAssetItems(Loader& owner) : loader(owner) {}
    ~OwnedAssetItems() {
        for (auto it = items.begin; it != items.end; ++it) if (it->control)
            fn<void (*)(void*)>(loader, strings, "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(it->control);
        if (items.begin) fn<void (*)(void*)>(loader, strings, "_ZdlPv")(items.begin);
    }
};
struct SubscriptionRequest {
    Loader& loader;
    // Pinned PurchaseRequest ABI, as used by wrapper-lite.
    alignas(16) std::array<unsigned char, 1024> storage{};
    SubscriptionRequest(Loader& owner, const Services& services) : loader(owner) {
        fn<void (*)(void*, const GuestShared*)>(loader, store,
            "_ZN17storeservicescore15PurchaseRequestC2ERKNSt6__ndk110shared_ptrINS_14RequestContextEEE")(storage.data(), &services.context);
    }
    ~SubscriptionRequest() {
        fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore15PurchaseRequestD2Ev")(storage.data());
    }
};

int64_t subscriptionSongID(const std::string& adam) {
    int64_t id = 0;
    const char* end = adam.data() + adam.size();
    auto parsed = std::from_chars(adam.data(), end, id);
    if (adam.size() > 19 || parsed.ec != std::errc{} || parsed.ptr != end || id <= 0)
        throw std::runtime_error("invalid catalog song ID");
    return id;
}

struct OwnedShared {
    Loader& loader;
    GuestShared value;
    explicit OwnedShared(Loader& owner) : loader(owner) {}
    ~OwnedShared() {
        if (value.control) fn<void (*)(void*)>(loader, strings, "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(value.control);
    }
};

// wrapper-lite's offline_available_impl: the second 16-byte subscription status
// record contains the offline state at byte 8 in the pinned Apple SDK.
bool hasOfflineChannel(Loader& loader, const Services& services) {
    OwnedShared fairplay(loader);
    callGuestReturn(loader.symbol(store, "_ZN17storeservicescore14RequestContext8fairPlayEv"), &fairplay.value, services.context.object);
    if (!fairplay.value.object) return false;
    struct { unsigned char *begin{}, *end{}, *capacity{}; } status;
    callGuestReturn(loader.symbol(store, "_ZN17storeservicescore8FairPlay21getSubscriptionStatusEv"), &status, fairplay.value.object);
    int state = 0;
    if (status.begin && status.end - status.begin >= 32) std::memcpy(&state, status.begin + 24, sizeof(state));
    if (status.begin) fn<void (*)(void*)>(loader, strings, "_ZdlPv")(status.begin);
    return state == 2 || state == 3;
}

DeviceAsset subscriptionAsset(Loader& loader, const Services& services, int64_t id) {
    SubscriptionRequest request(loader, services);
    auto object = request.storage.data();
    GuestString key("subDownload");
    const auto query = "salableAdamId=" + std::to_string(id) + "&price=0&pricingParameters=SUBS&productType=S";
    GuestString parameters(query.c_str());
    fn<void (*)(void*, bool)>(loader, store, "_ZN17storeservicescore15PurchaseRequest23setProcessDialogActionsEb")(object, false);
    fn<void (*)(void*, const GuestString*)>(loader, store, "_ZN17storeservicescore15PurchaseRequest12setURLBagKeyERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE")(object, &key);
    fn<void (*)(void*, const GuestString*)>(loader, store, "_ZN17storeservicescore15PurchaseRequest16setBuyParametersERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE")(object, &parameters);
    fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore15PurchaseRequest3runEv")(object);
    auto response = fn<const GuestShared* (*)(void*)>(loader, store, "_ZNK17storeservicescore15PurchaseRequest8responseEv")(object);
    if (!response || !response->object) return {};
    auto failure = fn<const GuestShared* (*)(void*)>(loader, store, "_ZN17storeservicescore16PurchaseResponse5errorEv")(response->object);
    if (failure && failure->object) return {};
    OwnedAssetItems songs(loader);
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore16PurchaseResponse5itemsEv"), &songs.items, response->object);
    if (songs.items.begin == songs.items.end || !songs.items.begin->object) return {};
    auto song = songs.items.begin->object;
    if (fn<int64_t (*)(void*)>(loader, store, "_ZNK17storeservicescore12PurchaseItem14itemIdentifierEv")(song) != id)
        throw std::runtime_error("subscription returned a different song");
    OwnedAssetItems assets(loader);
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore12PurchaseItem6assetsEv"), &assets.items, song);
    if (assets.items.begin == assets.items.end || !assets.items.end[-1].object) return {};
    // Upstream returns the final asset of the first item, including a direct
    // protected file for recordings that do not have an HLS master.
    auto asset = assets.items.end[-1].object;
    OwnedString flavor(loader), url(loader);
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore13PurchaseAsset6flavorEv"), flavor.bytes.data(), asset);
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore13PurchaseAsset3URLEv"), url.bytes.data(), asset);
    return {text(url.bytes.data(), 32 * 1024), flavor.value()};
}

DeviceAsset playbackAsset(Loader& loader, const Services& services, int64_t id) {
    // The SDK takes a borrowed NDK vector<string>. Keep the backing string
    // alive until requestAsset returns and retain its owning response via x8.
    OwnedString hls(loader, "HLS");
    auto begin = hls.bytes.data(), end = begin + hls.bytes.size();
    struct { const unsigned char *begin, *end, *capacity; } flavors{begin, end, end};
    OwnedShared response(loader);
    const bool interrupt = false;
    callGuestReturn(loader.symbol(music, "_ZN22SVPlaybackLeaseManager12requestAssetERKmRKNSt6__ndk16vectorINS2_12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEENS7_IS9_EEEERKb"),
                    &response.value, services.lease, &id, &flavors, &interrupt);
    if (!response.value.object || !fn<bool (*)(const void*)>(loader, music, "_ZNK23SVPlaybackAssetResponse13hasValidAssetEv")(response.value.object)) return {};
    auto asset = fn<const GuestShared* (*)(const void*)>(loader, music, "_ZNK23SVPlaybackAssetResponse13playbackAssetEv")(response.value.object);
    if (!asset || !asset->object) return {};
    OwnedString url(loader), flavor(loader);
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore13PlaybackAsset9URLStringEv"), url.bytes.data(), asset->object);
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore13PlaybackAsset6flavorEv"), flavor.bytes.data(), asset->object);
    return {text(url.bytes.data(), 32 * 1024), flavor.value()};
}

std::string deviceManifest(Loader& loader, const Services& services, const std::string& adam, DeviceManifestSession& session) {
    auto id = subscriptionSongID(adam);
    if (!session.offlineChannel.has_value()) session.offlineChannel = hasOfflineChannel(loader, services);
    auto asset = resolveDeviceAsset(*session.offlineChannel,
        [&] { return subscriptionAsset(loader, services, id); },
        [&] {
            session.requirePlaybackLease(
                fn<bool (*)(void*, const bool*)>(loader, music, "_ZN22SVPlaybackLeaseManager12requestLeaseERKb"),
                services.lease);
            return playbackAsset(loader, services, id);
        });
    const char* type = asset.flavor == "HLS" ? "hls" : "file";
    return "{\"adamId\":" + jsonString(adam) + ",\"m3u8\":" + (asset.url.empty() ? "null" : jsonString(asset.url))
        + ",\"streamType\":" + jsonString(type) + "}";
}
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
struct OwnedAccount {
    Loader& loader;
    GuestShared value;
    OwnedAccount(Loader& owner, const Services& services) : loader(owner) {
        callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore14RequestContext7accountEv"), &value, services.context.object);
    }
    ~OwnedAccount() {
        if (value.control) fn<void (*)(void*)>(loader, strings, "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(value.control);
    }
    explicit operator bool() const { return value.object && value.control; }
};
bool accountPresent(Loader& loader, const Services& services) {
    return static_cast<bool>(OwnedAccount(loader, services));
}
std::string accountName(Loader& loader, const void* account) {
    if (!account) return "";
    try {
        // These getters return references to the account's cached NDK strings.
        // Read only name fields; do not request tokens, network data or Contacts.
        auto first = text(fn<const void* (*)(const void*)>(loader, store, "_ZNK17storeservicescore7Account9firstNameEv")(account), 1024);
        auto last = text(fn<const void* (*)(const void*)>(loader, store, "_ZNK17storeservicescore7Account8lastNameEv")(account), 1024);
        auto name = first + (!first.empty() && !last.empty() ? " " : "") + last;
        auto valid = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(name.data()), name.size(), kCFStringEncodingUTF8, false);
        if (!valid) return "";
        CFRelease(valid);
        return name;
    } catch (const std::exception&) {
        // Missing/invalid metadata must not hide a valid signed-in status.
        return "";
    }
}
std::string accountStatus(Loader& loader, const Services& services) {
    OwnedAccount account(loader, services);
    return "{\"regions\":[" + (account ? jsonString(storefront(loader, services)) : "") +
        "],\"account_present\":" + (account ? "true" : "false") +
        ",\"account_name\":" + jsonString(account ? accountName(loader, account.value.object) : "") +
        ",\"backend\":\"macos-arm64\",\"native_sample_decryption\":true}";
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
        auto version = music_client::userAgent();
        header("X-Apple-Requesting-Bundle-Version", version.c_str());
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
    if (operation != "status" && operation != "tokens" && operation != "key" && operation != "manifest") throw std::runtime_error("unknown native API operation");
    if (operation == "manifest") (void)subscriptionSongID(adam);
    if (operation == "key" && (adam.empty() || adam.size() > 19 || !std::all_of(adam.begin(), adam.end(), [](char c) { return c >= '0' && c <= '9'; }))) throw std::runtime_error("invalid adamId");
    if (operation == "key" && (uri.rfind("skd://", 0) != 0 || uri.size() > 8192 || (adam != "0" && uri == "skd://itunes.apple.com/P000000000/s1/e1"))) throw std::runtime_error("invalid key URI");
    if (operation == "tokens" && isatty(STDOUT_FILENO)) throw std::runtime_error("token exchange is an internal pipe operation");
    setGuestLogging(false);
    auto services = initializeServices(loader, directory, profileIdentity(directory));
    bool account = accountPresent(loader, services);
    if (operation == "status") return "{\"regions\":[" + (account ? jsonString(storefront(loader, services)) : "") + "],\"account_present\":" + (account ? "true" : "false") + ",\"backend\":\"macos-arm64\",\"native_sample_decryption\":true}";
    if (!account) throw std::runtime_error("no saved account; sign in first");
    if (operation == "manifest") {
        DeviceManifestSession manifest;
        return deviceManifest(loader, services, adam, manifest);
    }
    if (operation == "tokens") return tokens(loader, services);
    GuestString identifier(adam.c_str()), key_uri(uri.c_str()), format("com.apple.streamingkeydelivery"), version("1"), server("https://play.itunes.apple.com/WebObjects/MZPlay.woa/music/fps"), protocol("simplified"), certificate(fairplayCertificate);
    const void* arguments[] = {services.foothill, &identifier, &identifier, &key_uri, &format, &version, &server, &protocol, &certificate};
    GuestShared key;
    callGuestReturn9(loader.symbol(music, "_ZN21SVFootHillSessionCtrl16getPersistentKeyERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_S8_S8_S8_S8_S8_S8_"), &key, arguments);
    auto content = text(key.object);
    if (content.empty()) throw std::runtime_error("native FairPlay key retrieval returned empty");
    return "{\"adamId\":" + jsonString(adam) + ",\"keyUri\":" + jsonString(uri) + ",\"contentKey\":" + jsonString(content) + "}";
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
void writeFrame(WorkerStatus status, const Bytes& result) {
    Bytes header;
    appendWord(header, static_cast<uint32_t>(result.size() + 1));
    header.push_back(static_cast<uint8_t>(status));
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

void nativeDecryptionWorker(Loader& loader, const std::string& directory, bool embedded) {
    if (isatty(STDIN_FILENO) || isatty(STDOUT_FILENO)) throw std::runtime_error("sample worker requires private pipes");
    setGuestLogging(false);
    auto services = initializeServices(loader, directory, profileIdentity(directory));
    if (!embedded && !accountPresent(loader, services)) throw std::runtime_error("no saved account");
    std::vector<std::unique_ptr<NativeContext>> contexts;
    DeviceManifestSession manifest;
    writeFrame(WorkerStatus::success, Bytes{'N', 'A', 'D', '2'});
    Bytes request;
    while (readFrame(request)) {
        try {
            Frame input(request);
            auto operation = input.word();
            Bytes response;
            if (operation == 1) {
                if (!accountPresent(loader, services)) throw NoSavedAccount{};
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
                    // Each sample has an independent aligned encrypted prefix and a clear tail.
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
            } else if (embedded && operation == 4) {
                input.end();
                auto json = accountStatus(loader, services);
                response.assign(json.begin(), json.end());
            } else if (embedded && operation == 5) {
                input.end();
                if (!accountPresent(loader, services)) throw NoSavedAccount{};
                auto json = tokens(loader, services);
                response.assign(json.begin(), json.end());
            } else if (embedded && operation == 6) {
                auto adam = input.string(19);
                input.end();
                (void)subscriptionSongID(adam);
                if (!accountPresent(loader, services)) throw NoSavedAccount{};
                auto json = deviceManifest(loader, services, adam, manifest);
                response.assign(json.begin(), json.end());
            } else if (embedded && (operation == profileReadOperation || operation == profileRefreshOperation)) {
                input.end();
                auto json = currentUserProfile(loader, services, operation == profileRefreshOperation);
                response.assign(json.begin(), json.end());
            } else throw std::runtime_error("unknown native sample operation");
            writeFrame(WorkerStatus::success, response);
        } catch (const NoSavedAccount&) {
            writeFrame(WorkerStatus::noAccount, {});
        } catch (const std::exception&) {
            // No keys, input samples, account identifiers, or guest errors enter diagnostics.
            writeFrame(WorkerStatus::rejected, {});
        }
    }
}

std::string probeAccountMetadataAbi(Loader& loader, const std::string& directory) {
    setGuestLogging(false);
    auto services = initializeServices(loader, directory, profileIdentity(directory)); (void)services;
    // The pinned Account constructor uses 192 bytes. An independent canary
    // after that object catches ABI drift; no saved or real account is changed.
    alignas(16) std::array<unsigned char, 256> storage{};
    std::fill(storage.begin() + 192, storage.end(), 0xa5);
    fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore7AccountC2Ev")(storage.data());
    auto destroy = [&] { fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore7AccountD2Ev")(storage.data()); };
    try {
        if (!accountName(loader, storage.data()).empty() || !accountName(loader, nullptr).empty()) throw std::runtime_error("empty native account name did not fall back");
        auto set = [&](const std::string& first, const std::string& last) {
            OwnedString given(loader, first), family(loader, last);
            fn<void (*)(void*, const void*)>(loader, store, "_ZN17storeservicescore7Account12setFirstNameERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE")(storage.data(), given.bytes.data());
            fn<void (*)(void*, const void*)>(loader, store, "_ZN17storeservicescore7Account11setLastNameERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE")(storage.data(), family.bytes.data());
        };
        std::string first = "Renée " + std::string(80, 'x'), last = "山田";
        set(first, last);
        if (accountName(loader, storage.data()) != first + " " + last) throw std::runtime_error("native account name round trip failed");
        set("", last);
        if (accountName(loader, storage.data()) != last) throw std::runtime_error("partial native account name was lost");
        set(std::string(1025, 'x'), last);
        if (!accountName(loader, storage.data()).empty()) throw std::runtime_error("oversized native account name was accepted");
        set(std::string(1, char(0xff)), last);
        if (!accountName(loader, storage.data()).empty()) throw std::runtime_error("invalid native account UTF-8 was accepted");
        if (!std::all_of(storage.begin() + 192, storage.end(), [](unsigned char c) { return c == 0xa5; })) throw std::runtime_error("native Account layout changed");
    } catch (...) { destroy(); throw; }
    destroy();
    return "{\"name_round_trip\":true,\"empty_name_fallback\":true,\"partial_name\":true,\"bounded_name\":true,\"invalid_utf8_fallback\":true,\"credentials_submitted\":false}";
}
}
