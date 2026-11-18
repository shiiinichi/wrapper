#include "user_profile.h"
#include "profile_metadata.h"
#include "worker_protocol.h"
#include "elf_loader.h"
#include "guest_call.h"
#include "format_bridge.h"
#include "services.h"
#include <CoreFoundation/CoreFoundation.h>
#include <array>
#include <cstring>
#include <stdexcept>

namespace native_poc {
namespace {
constexpr const char* store = "libstoreservicescore.so";
constexpr const char* platform = "libmediaplatform.so";
template<class F> F fn(Loader& loader, const char* library, const char* name) {
    auto address = loader.symbol(library, name);
    for (const auto& image : loader.images())
        if (image->report.library == library && image->executableAddress(reinterpret_cast<uintptr_t>(address)))
            return reinterpret_cast<F>(address);
    throw std::runtime_error("profile SDK function is not executable");
}
struct Shared {
    Loader& loader;
    GuestShared value;
    explicit Shared(Loader& owner) : loader(owner) {}
    ~Shared() {
        if (value.control) fn<void (*)(void*)>(loader, "libc++_shared.so",
            "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(value.control);
    }
};
int64_t activeID(Loader& loader, const Services& services) {
    Shared account(loader);
    callGuestReturn(reinterpret_cast<void*>(fn<void (*)()>(loader, store,
        "_ZNK17storeservicescore14RequestContext7accountEv")), &account.value, services.context.object);
    if (!account.value.object || !account.value.control) throw NoSavedAccount{};
    return fn<int64_t (*)(const void*)>(loader, store, "_ZNK17storeservicescore7Account4DSIDEv")(account.value.object);
}
std::string displayText(const char* bytes, size_t size, size_t limit) {
    if (!bytes || !size || size > limit) return {};
    std::string value(bytes, size);
    if (!validProfileText(value, limit)) return {};
    auto utf8 = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(bytes), size,
                                        kCFStringEncodingUTF8, false);
    if (!utf8) return {};
    CFRelease(utf8);
    return value;
}
std::string profileHandle(Loader& loader, const void* profile) {
    auto object = fn<const unsigned char* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore11UserProfile6handleEv")(profile);
    if (!object) return {};
    size_t size = object[0] >> 1;
    const char* bytes = reinterpret_cast<const char*>(object + 1);
    if (object[0] & 1) {
        std::memcpy(&size, object + 8, 8);
        std::memcpy(&bytes, object + 16, 8);
    } else if (size > 22) return {};
    return displayText(bytes, size, maximumProfileHandle);
}
std::string profileName(Loader& loader, const void* profile) {
    // name() returns a borrowed shared_ptr<Data> reference, NOT an NDK string.
    auto name = fn<const GuestShared* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore11UserProfile4nameEv")(profile);
    if (!name || !name->object) return {};
    auto size = fn<size_t (*)(const void*)>(loader, platform, "_ZNK13mediaplatform4Data6lengthEv")(name->object);
    if (!size || size > maximumProfileName) return {};
    auto bytes = fn<const char* (*)(const void*)>(loader, platform, "_ZNK13mediaplatform4Data5bytesEv")(name->object);
    return displayText(bytes, size, maximumProfileName);
}
std::string imageJSON(const CFDictionaryRef* reference) {
    if (!reference || !*reference || CFGetTypeID(*reference) != CFDictionaryGetTypeID()) return "null";
    auto image = *reference;
    auto url = CFDictionaryGetValue(image, CFSTR("url"));
    if (!url || CFGetTypeID(url) != CFStringGetTypeID() ||
        CFStringGetLength(static_cast<CFStringRef>(url)) > static_cast<CFIndex>(maximumProfileURL)) return "null";
    std::array<char, maximumProfileURL + 1> bytes{};
    if (!CFStringGetCString(static_cast<CFStringRef>(url), bytes.data(), bytes.size(), kCFStringEncodingUTF8))
        return "null";
    // CF strings can contain embedded NULs. Do not return a truncated URL.
    auto urlLength = CFStringGetLength(static_cast<CFStringRef>(url));
    if (static_cast<size_t>(urlLength) != std::strlen(bytes.data()) || !validProfileImageURL(bytes.data()))
        return "null";
    std::string result = "{\"url\":" + jsonString(bytes.data());
    for (auto key : {CFSTR("width"), CFSTR("height")}) {
        auto value = CFDictionaryGetValue(image, key);
        if (!value) continue;
        int64_t dimension = 0;
        if (CFGetTypeID(value) != CFNumberGetTypeID() ||
            !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &dimension) ||
            dimension <= 0 || dimension > maximumProfileDimension) return "null";
        result += key == CFSTR("width") ? ",\"width\":" : ",\"height\":";
        result += std::to_string(dimension);
    }
    return result + "}";
}
std::string profileJSON(Loader& loader, const void* profile, int64_t account) {
    const std::string missing = "{\"profile_present\":false,\"name\":null,\"handle\":null,\"image\":null}";
    if (!profile || account <= 0) return missing;
    auto id = fn<const int64_t* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore11UserProfile11profileDSIDEv")(profile);
    if (!id || *id != account) return missing;
    auto optional = [](const std::string& value) { return value.empty() ? "null" : jsonString(value); };
    auto image = fn<const CFDictionaryRef* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore11UserProfile12profileImageEv")(profile);
    return "{\"profile_present\":true,\"name\":" + optional(profileName(loader, profile)) +
        ",\"handle\":" + optional(profileHandle(loader, profile)) + ",\"image\":" + imageJSON(image) + "}";
}
void refreshProfile(Loader& loader, const Services& services, int64_t id) {
    // Pinned UserProfileRequest: ComplexRequest(160), context(16), DSID(8), response(16).
    alignas(16) std::array<unsigned char, 200> request{};
    fn<void (*)(void*, const GuestShared*, const uint64_t*)>(loader, store,
        "_ZN17storeservicescore18UserProfileRequestC2ERKNSt6__ndk110shared_ptrINS_14RequestContextEEERKm")(
            request.data(), &services.context, reinterpret_cast<const uint64_t*>(&id));
    struct Destroy {
        Loader& loader; void* object;
        ~Destroy() { fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore18UserProfileRequestD2Ev")(object); }
    } destroy{loader, request.data()};
    fn<void (*)(void*, double)>(loader, store,
        "_ZN17storeservicescore18UserProfileRequest14runWithTimeoutENSt6__ndk16chrono8durationIdNS1_5ratioILl1ELl1EEEEE")(
            request.data(), profileSDKTimeoutSeconds);
    auto response = fn<const GuestShared* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore18UserProfileRequest8responseEv")(request.data());
    if (!response || !response->object) throw std::runtime_error("profile refresh returned no response");
    auto error = fn<const GuestShared* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore19UserProfileResponse5errorEv")(response->object);
    if ((error && error->object) || !fn<bool (*)(void*)>(loader, store,
        "_ZN17storeservicescore19UserProfileResponse20isSuccessfulResponseEv")(response->object))
        throw std::runtime_error("profile refresh failed");
}
} // namespace

std::string currentUserProfile(Loader& loader, const Services& services, bool refresh) {
    const auto account = activeID(loader, services);
    if (account <= 0) throw NoSavedAccount{};
    if (refresh) refreshProfile(loader, services, account);
    Shared profile(loader);
    callGuestReturn(reinterpret_cast<void*>(fn<void (*)()>(loader, store,
        "_ZNK17storeservicescore14RequestContext11userProfileEv")), &profile.value, services.context.object);
    auto result = profileJSON(loader, profile.value.control ? profile.value.object : nullptr, account);
    // Account operations are serialized by the host; also fail closed if the SDK changes identity.
    if (activeID(loader, services) != account) throw std::runtime_error("profile account changed");
    if (result.size() > maximumProfileReply) throw std::runtime_error("profile result too large");
    return result;
}

std::string probeUserProfileAbi(Loader& loader, const std::string& directory) {
    setGuestLogging(false);
    auto services = initializeServices(loader, directory);
    const char* step = "optional getters and identity";
    auto require = [&](bool pass) { if (!pass) throw std::runtime_error(std::string("user profile ABI: ") + step); };
    alignas(16) std::array<unsigned char, 256> profile{};
    std::fill(profile.begin() + 152, profile.end(), 0xa5);
    fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore11UserProfileC2Ev")(profile.data());
    struct DestroyProfile {
        Loader& loader; void* object;
        ~DestroyProfile() { fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore11UserProfileD2Ev")(object); }
    } destroyProfile{loader, profile.data()};
    int64_t id = 42;
    fn<void (*)(void*, const int64_t*)>(loader, store, "_ZN17storeservicescore11UserProfile14setProfileDSIDERKl")(
        profile.data(), &id);
    auto getImage = fn<const CFDictionaryRef* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore11UserProfile12profileImageEv");
    auto setImage = fn<void (*)(void*, const CFDictionaryRef*)>(loader, store,
        "_ZN17storeservicescore11UserProfile15setProfileImageERKPK14__CFDictionary");
    auto empty = getImage(profile.data());
    require(empty && !*empty && imageJSON(nullptr) == "null" && imageJSON(empty) == "null");
    require(profileName(loader, profile.data()).empty() && profileHandle(loader, profile.data()).empty());
    require(profileJSON(loader, profile.data(), 84).find("\"profile_present\":false") != std::string::npos);
    step = "name Data";
    for (const auto& value : {std::string("Renée \"quoted\" 山田 🎵"), std::string(maximumProfileName, 'x'),
                              std::string(maximumProfileName + 1, 'x'), std::string(1, char(0xff)), std::string("bad\nname")}) {
        alignas(16) std::array<unsigned char, 64> data{};
        std::fill(data.begin() + 40, data.end(), 0xa5);
        fn<void (*)(void*, const void*, size_t, bool)>(loader, platform, "_ZN13mediaplatform4DataC2EPKvmb")(
            data.data(), value.data(), value.size(), true);
        // A non-owning alias shared_ptr is a valid guest reference. The probe
        // owns this Data on its stack and destroys it after the getter returns.
        GuestShared alias{data.data(), nullptr};
        fn<void (*)(void*, const GuestShared*)>(loader, store,
            "_ZN17storeservicescore11UserProfile7setNameERKNSt6__ndk110shared_ptrIN13mediaplatform4DataEEE")(
                profile.data(), &alias);
        auto actual = profileName(loader, profile.data());
        fn<void (*)(void*)>(loader, platform, "_ZN13mediaplatform4DataD2Ev")(data.data());
        require(actual == displayText(value.data(), value.size(), maximumProfileName));
        require(std::all_of(data.begin() + 40, data.end(), [](unsigned char c) { return c == 0xa5; }));
    }
    GuestShared noName;
    fn<void (*)(void*, const GuestShared*)>(loader, store,
        "_ZN17storeservicescore11UserProfile7setNameERKNSt6__ndk110shared_ptrIN13mediaplatform4DataEEE")(profile.data(), &noName);
    step = "handle string";
    for (const auto& value : {std::string("fixture.handle"), std::string(maximumProfileHandle, 'x'),
                              std::string(maximumProfileHandle + 1, 'x'), std::string(1, char(0xff))}) {
        GuestString handle(value.c_str());
        fn<void (*)(void*, const GuestString*)>(loader, store,
            "_ZN17storeservicescore11UserProfile9setHandleERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE")(
                profile.data(), &handle);
        require(profileHandle(loader, profile.data()) == displayText(value.data(), value.size(), maximumProfileHandle));
    }
    step = "image dictionary";
    auto image = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    struct Release { CFTypeRef value; ~Release() { CFRelease(value); } } releaseImage{image};
    CFDictionarySetValue(image, CFSTR("url"), CFSTR("https://is1-ssl.mzstatic.com/fixture/{w}x{h}{c}.{f}"));
    int64_t edge = 512;
    auto number = CFNumberCreate(nullptr, kCFNumberSInt64Type, &edge);
    CFDictionarySetValue(image, CFSTR("width"), number);
    CFDictionarySetValue(image, CFSTR("height"), number);
    CFRelease(number);
    CFDictionaryRef reference = image;
    setImage(profile.data(), &reference);
    require(imageJSON(getImage(profile.data())).find("\"width\":512") != std::string::npos);
    CFDictionaryRef absent = nullptr;
    setImage(profile.data(), &absent);
    require(*getImage(profile.data()) != nullptr); // The SDK's null setter preserves the previous image.
    CFDictionarySetValue(image, CFSTR("width"), kCFBooleanTrue);
    setImage(profile.data(), &reference);
    require(imageJSON(getImage(profile.data())) == "null");
    CFDictionaryRemoveValue(image, CFSTR("width"));
    setImage(profile.data(), &reference);
    require(imageJSON(getImage(profile.data())) != "null");
    for (auto url : {CFSTR("http://is1-ssl.mzstatic.com/fixture"), CFSTR("https://is1-ssl.mzstatic.com/fixture?token=secret"),
                     CFSTR("https://example.invalid/fixture"), CFSTR("https://user@is1-ssl.mzstatic.com/fixture")}) {
        CFDictionarySetValue(image, CFSTR("url"), url);
        setImage(profile.data(), &reference);
        require(imageJSON(getImage(profile.data())) == "null");
    }
    require(std::all_of(profile.begin() + 152, profile.end(), [](unsigned char c) { return c == 0xa5; }));
    step = "request layout";
    alignas(16) std::array<unsigned char, 256> request{};
    std::fill(request.begin() + 200, request.end(), 0xa5);
    uint64_t account = 42;
    fn<void (*)(void*, const GuestShared*, const uint64_t*)>(loader, store,
        "_ZN17storeservicescore18UserProfileRequestC2ERKNSt6__ndk110shared_ptrINS_14RequestContextEEERKm")(
            request.data(), &services.context, &account);
    auto response = fn<const GuestShared* (*)(const void*)>(loader, store,
        "_ZNK17storeservicescore18UserProfileRequest8responseEv")(request.data());
    require(response && !response->object);
    fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore18UserProfileRequestD2Ev")(request.data());
    require(std::all_of(request.begin() + 200, request.end(), [](unsigned char c) { return c == 0xa5; }));
    return "{\"name_data_round_trip\":true,\"bounded_fields\":true,\"identity_matching\":true,\"optional_image\":true,"
        "\"validated_image\":true,\"null_image_setter_preserves_value\":true,\"request_layout\":true,\"credentials_submitted\":false}";
}
} // namespace native_poc
