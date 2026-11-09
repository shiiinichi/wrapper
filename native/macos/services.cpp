#include "services.h"
#include "elf_loader.h"
#include "jni_host.h"
#include "posix_bridge.h"
#include "guest_call.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace native_poc {
namespace {
constexpr const char* music = "libandroidappmusic.so";
constexpr const char* store = "libstoreservicescore.so";
using Shared = GuestShared;
using String = GuestString;
void stage(const char* name) { std::fprintf(stderr, "native-poc: bootstrap %s\n", name); }
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void* entry(Loader& loader, const char* library, const char* name) {
    void* address = loader.symbol(library, name);
    for (const auto& image : loader.images())
        if (image->report.library == library && image->executableAddress(reinterpret_cast<uintptr_t>(address))) return address;
    throw std::runtime_error(std::string("bootstrap function is not executable: ") + name);
}
template<class Function> Function fn(Loader& loader, const char* library, const char* name) {
    return reinterpret_cast<Function>(entry(loader, library, name));
}
void setString(Loader& loader, const char* symbol, void* object, const char* text) {
    String value(text);
    fn<void (*)(void*, const String*)>(loader, store, symbol)(object, &value);
}
void checkError(Loader& loader, void* error, const char* operation) {
    auto code = fn<const int* (*)(void*)>(loader, store, "_ZNK17storeservicescore19StoreErrorCondition9errorCodeEv")(error);
    int value = code ? *code : -1;
    if (value) throw std::runtime_error(std::string(operation) + " returned StoreErrorCondition " + std::to_string(value));
    fn<void (*)(void*)>(loader, store, "_ZN17storeservicescore19StoreErrorConditionD2Ev")(error);
}
[[noreturn]] void unexpectedDialog(long, Shared*, Shared*) {
    std::fputs("native-poc: bootstrap unexpectedly requested a dialog\n", stderr); std::_Exit(88);
}
[[noreturn]] void unexpectedCredentials(Shared*, Shared*) {
    std::fputs("native-poc: bootstrap unexpectedly requested credentials\n", stderr); std::_Exit(88);
}
std::string stringValue(const void* object) {
    require(object, "guest string is null");
    const auto* bytes = static_cast<const unsigned char*>(object);
    size_t size; const char* data;
    if (bytes[0] & 1) {
        std::memcpy(&size, bytes + 8, 8); std::memcpy(&data, bytes + 16, 8);
    } else { size = bytes[0] >> 1; require(size <= 22, "guest short string is too large"); data = reinterpret_cast<const char*>(bytes + 1); }
    require(size <= 1024 * 1024 && (data || !size), "guest string has an invalid length");
    return std::string(data, size);
}
} // namespace

Services initializeServices(Loader& loader, const std::string& data_directory, const std::string& device_identity) {
    require(!data_directory.empty(), "service bootstrap requires --data-dir");
    std::filesystem::create_directories(data_directory);
    auto base = std::filesystem::canonical(data_directory).string();
    std::filesystem::permissions(base, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    setRuntimeDirectory(base);
    loader.load(music);
    loader.relocate();
    installCrashDiagnostics(loader);
    loader.prepareExecution();
    loader.initialize();

    stage("device-config");
    // Bootstrap probes use a fixed identifier; authentication supplies a
    // separate randomly generated identifier persisted in its own profile.
    String identity(device_identity.c_str()), empty("");
    fn<void (*)(const String*)>(loader, music,
        "_ZN14FootHillConfig6configERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE")(&identity);
    Shared guid;
    callGuestReturn(entry(loader, store, "_ZN17storeservicescore10DeviceGUID8instanceEv"), &guid);
    require(guid.object && guid.control, "DeviceGUID::instance returned empty");
    alignas(16) std::array<unsigned char, 88> error{};
    unsigned api = 29; bool fairplay = true;
    callGuestReturn(entry(loader, store,
        "_ZN17storeservicescore10DeviceGUID9configureERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_RKjRKb"),
        error.data(), guid.object, &identity, &empty, &api, &fairplay);
    checkError(loader, error.data(), "DeviceGUID::configure");
    require(fn<bool (*)(void*)>(loader, store, "_ZN17storeservicescore10DeviceGUID12isConfiguredEv")(guid.object), "DeviceGUID is not configured");

    stage("request-context-create");
    auto database = base + "/mpl_db"; String path(database.c_str());
    Shared context;
    callGuestReturn(entry(loader, music,
        "_ZNSt6__ndk110shared_ptrIN17storeservicescore14RequestContextEE11make_sharedIJRNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEEEES3_DpOT_"), &context, &path);
    require(context.object && context.control, "RequestContext factory returned empty");

    // Exact emplace allocation used by the pinned library's JNI factory at
    // 0x1bc17c: 480 bytes, counters zero, vtable+16, object+32. Construction and
    // eventual destruction use Android functions, never Darwin std::__1 types.
    auto* allocation = static_cast<unsigned char*>(std::calloc(1, 480));
    require(allocation, "RequestContextConfig allocation failed");
    auto* table = static_cast<unsigned char*>(loader.symbol(music,
        "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore20RequestContextConfigENS_9allocatorIS2_EEEE"));
    require(table, "RequestContextConfig vtable missing");
    *reinterpret_cast<void**>(allocation) = table + 16;
    Shared config{allocation + 32, allocation};
    fn<void (*)(void*)>(loader, music, "_ZN17storeservicescore20RequestContextConfigC2Ev")(config.object);
    setString(loader, "_ZN17storeservicescore20RequestContextConfig20setBaseDirectoryPathERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE", config.object, base.c_str());
    const char* setters[] = {
        "19setClientIdentifier", "20setVersionIdentifier", "21setPlatformIdentifier", "17setProductVersion",
        "14setDeviceModel", "15setBuildVersion", "19setLocaleIdentifier", "21setLanguageIdentifier"};
    const char* values[] = {"com.apple.android.music", "4.9.3", "Android", "10", "NativeMacPoC", "native-poc", "en_US", "en"};
    for (size_t i = 0; i < 8; ++i) {
        std::string symbol = std::string("_ZN17storeservicescore20RequestContextConfig") + setters[i] +
            "ERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
        setString(loader, symbol.c_str(), config.object, values[i]);
    }
    stage("request-context-init");
    fn<void (*)(const Shared*)>(loader, music,
        "_ZN21RequestContextManager9configureERKNSt6__ndk110shared_ptrIN17storeservicescore14RequestContextEEE")(&context);
    callGuestReturn(entry(loader, store, "_ZN17storeservicescore14RequestContext4initERKNSt6__ndk110shared_ptrINS_20RequestContextConfigEEE"),
        error.data(), context.object, &config);
    checkError(loader, error.data(), "RequestContext::init");
    setString(loader, "_ZN17storeservicescore14RequestContext24setFairPlayDirectoryPathERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE", context.object, base.c_str());
    alignas(8) std::array<unsigned char, 24> directory{};
    callGuestReturn(entry(loader, store, "_ZNK17storeservicescore14RequestContext21fairPlayDirectoryPathEv"), directory.data(), context.object);
    require(stringValue(directory.data()) == base, "RequestContext FairPlay directory did not round trip");
    fn<void (*)(void*)>(loader, "libc++_shared.so", "_ZNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED2Ev")(directory.data());
    Shared configured;
    callGuestReturn(entry(loader, music, "_ZN21RequestContextManager14sharedInstanceEv"), &configured);
    require(configured.object == context.object, "RequestContextManager returned a different context");

    stage("presentation-interface");
    Shared presentation;
    callGuestReturn(entry(loader, music, "_ZNSt6__ndk110shared_ptrIN20androidstoreservices28AndroidPresentationInterfaceEE11make_sharedIJEEES3_DpOT_"), &presentation);
    require(presentation.object && presentation.control, "PresentationInterface factory returned empty");
    fn<void (*)(void*, void (*)(long, Shared*, Shared*))>(loader, music,
        "_ZN20androidstoreservices28AndroidPresentationInterface16setDialogHandlerEPFvlNSt6__ndk110shared_ptrIN17storeservicescore14ProtocolDialogEEENS2_INS_36AndroidProtocolDialogResponseHandlerEEEE")(presentation.object, unexpectedDialog);
    fn<void (*)(void*, void (*)(Shared*, Shared*))>(loader, music,
        "_ZN20androidstoreservices28AndroidPresentationInterface21setCredentialsHandlerEPFvNSt6__ndk110shared_ptrIN17storeservicescore18CredentialsRequestEEENS2_INS_33AndroidCredentialsResponseHandlerEEEE")(presentation.object, unexpectedCredentials);
    fn<void (*)(void*, const Shared*)>(loader, store,
        "_ZN17storeservicescore14RequestContext24setPresentationInterfaceERKNSt6__ndk110shared_ptrINS_21PresentationInterfaceEEE")(context.object, &presentation);

    stage("fairplay-services");
    Shared fairplay_service;
    callGuestReturn(entry(loader, store, "_ZN17storeservicescore14RequestContext8fairPlayEv"), &fairplay_service, context.object);
    require(fairplay_service.object && fairplay_service.control, "RequestContext FairPlay service returned empty");
    auto* foothill = fn<void* (*)()>(loader, music, "_ZN21SVFootHillSessionCtrl8instanceEv")();
    require(foothill, "FootHill session controller returned empty");

    stage("lease-manager");
    // Empty Android std::function representations are intentional before login.
    // The pinned constructor checks the pointer at +32 and accepts no callbacks.
    // Never pass a Darwin std::function across the Android C++ ABI boundary.
    alignas(16) static std::array<unsigned char, 48> no_callback{};
    alignas(16) static std::array<unsigned char, 256> lease{};
    fn<void (*)(void*, const void*, const void*)>(loader, music,
        "_ZN22SVPlaybackLeaseManagerC2ERKNSt6__ndk18functionIFvRKiEEERKNS1_IFvRKNS0_10shared_ptrIN17storeservicescore19StoreErrorConditionEEEEEE")(lease.data(), no_callback.data(), no_callback.data());
    auto* session = reinterpret_cast<const Shared*>(lease.data());
    require(session->object && session->control, "PlaybackLeaseManager has no lease session");
    bool refresh = false;
    fn<void (*)(void*, const bool*)>(loader, music, "_ZN22SVPlaybackLeaseManager25refreshLeaseAutomaticallyERKb")(lease.data(), &refresh);
    stage("services-ready");
    std::string evidence = "{\"device_configured\":true,\"request_context_initialized\":true,\"context_round_trip\":true,"
        "\"presentation_interface\":true,\"fairplay_service\":true,\"foothill_controller\":true,\"lease_session\":true,"
        "\"automatic_lease_refresh\":false,\"lease_requested\":false,\"authenticated\":false,\"process_owned_lifetime\":true}";
    return {context, presentation, evidence};
}
std::string probeServices(Loader& loader, const std::string& directory) { return initializeServices(loader, directory).evidence; }
} // namespace native_poc
