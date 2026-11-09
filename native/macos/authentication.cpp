#include "authentication.h"
#include "services.h"
#include "guest_call.h"
#include "elf_loader.h"
#include "auth_input.h"
#include "format_bridge.h"
#include "network_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <cstring>
#include <vector>

namespace native_poc {
namespace {
constexpr const char* music = "libandroidappmusic.so";
constexpr const char* store = "libstoreservicescore.so";
Loader* active_loader = nullptr;
GuestShared presentation;
unsigned credential_requests = 0;
unsigned verification_requests = 0;
unsigned protocol_dialogs = 0;
bool allow_credentials_prompt = true;
bool preflight_mode = true, cancelled = false;
bool input_error = false;
LoginInput* login_input = nullptr;
std::string prompt_script;
template<class F> F fn(const char* library, const char* name) {
    void* address = active_loader->symbol(library, name);
    for (const auto& image : active_loader->images())
        if (image->report.library == library && image->executableAddress(reinterpret_cast<uintptr_t>(address))) return reinterpret_cast<F>(address);
    throw std::runtime_error("authentication function is not executable");
}
GuestShared credentialResponse(const std::string& user, const std::string& password, bool submit) {
    auto* storage = static_cast<unsigned char*>(std::calloc(1, 80));
    if (!storage) std::_Exit(89);
    auto* table = static_cast<unsigned char*>(active_loader->symbol(music,
        "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore19CredentialsResponseENS_9allocatorIS2_EEEE"));
    *reinterpret_cast<void**>(storage) = table + 16;
    GuestShared response{storage + 24, storage};
    fn<void (*)(void*)>(store, "_ZN17storeservicescore19CredentialsResponseC1Ev")(response.object);
    if (submit) {
        GuestString name(user.c_str()), secret(password.c_str());
        fn<void (*)(void*, const GuestString*)>(store, "_ZN17storeservicescore19CredentialsResponse11setUserNameERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE")(response.object, &name);
        fn<void (*)(void*, const GuestString*)>(store, "_ZN17storeservicescore19CredentialsResponse11setPasswordERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE")(response.object, &secret);
    }
    fn<void (*)(void*, int)>(store, "_ZN17storeservicescore19CredentialsResponse15setResponseTypeENS0_12ResponseTypeE")(response.object, submit ? 2 : 1);
    return response;
}
void credentials(GuestShared* request, GuestShared*) {
    ++credential_requests;
    bool needs_code = fn<bool (*)(void*)>(store, "_ZNK17storeservicescore18CredentialsRequest28requiresHSA2VerificationCodeEv")(request->object);
    std::fputs(needs_code ? "native-poc: verification code requested\n" : "native-poc: credentials requested\n", stderr);
    bool submit = !preflight_mode;
    std::string password, code;
    try {
        if (submit && needs_code) {
            ++verification_requests;
            submit = login_input && !login_input->password.empty() && verification_requests <= 3 && promptVerification(prompt_script, code);
        } else if (submit) {
            // A rejected password never triggers an automatic resubmission loop.
            submit = allow_credentials_prompt && promptLogin(prompt_script, *login_input);
            allow_credentials_prompt = false;
        }
    } catch (const std::exception&) {
        // Native exceptions must never unwind through unregistered ELF frames.
        input_error = true; submit = false;
    }
    if (submit) password = login_input->password + code;
    else cancelled = true;
    auto response = credentialResponse(submit ? login_input->user : "", password, submit);
    eraseSecret(password); eraseSecret(code);
    fn<void (*)(void*, const GuestShared*)>(music,
        "_ZN20androidstoreservices28AndroidPresentationInterface25handleCredentialsResponseERKNSt6__ndk110shared_ptrIN17storeservicescore19CredentialsResponseEEE")(presentation.object, &response);
    fn<void (*)(void*)>("libc++_shared.so", "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(response.control);
}
std::string guestText(const void* object) {
    const auto* bytes = static_cast<const unsigned char*>(object);
    if (!bytes) return "";
    size_t size; const char* text;
    if (bytes[0] & 1) { std::memcpy(&size, bytes + 8, 8); std::memcpy(&text, bytes + 16, 8); }
    else { size = bytes[0] >> 1; text = reinterpret_cast<const char*>(bytes + 1); if (size > 22) throw std::runtime_error("invalid guest short string"); }
    if (size > 65536 || (!text && size)) throw std::runtime_error("invalid guest dialog text");
    return std::string(text, size);
}
void dialog(long identifier, GuestShared* request, GuestShared*) {
    ++protocol_dialogs;
    std::fputs("native-poc: protocol dialog requested\n", stderr);
    int selected = -1;
    struct Vector { GuestShared* begin; GuestShared* end; GuestShared* capacity; };
    const Vector* buttons = nullptr;
    try {
        buttons = fn<const Vector* (*)(void*)>(store, "_ZNK17storeservicescore14ProtocolDialog7buttonsEv")(request->object);
        auto begin = reinterpret_cast<uintptr_t>(buttons->begin), end = reinterpret_cast<uintptr_t>(buttons->end);
        if (end < begin || (end - begin) % sizeof(GuestShared) || (end - begin) / sizeof(GuestShared) > 32) throw std::runtime_error("invalid dialog button list");
        std::vector<std::string> labels;
        for (auto* button = buttons->begin; button != buttons->end; ++button)
            labels.push_back(guestText(fn<const void* (*)(void*)>(store, "_ZNK17storeservicescore14ProtocolButton5titleEv")(button->object)));
        if (!preflight_mode) selected = promptDialog(prompt_script,
            guestText(fn<const void* (*)(void*)>(store, "_ZNK17storeservicescore14ProtocolDialog5titleEv")(request->object)),
            guestText(fn<const void* (*)(void*)>(store, "_ZNK17storeservicescore14ProtocolDialog7messageEv")(request->object)), labels);
    } catch (const std::exception&) { input_error = true; }
    auto* storage = static_cast<unsigned char*>(std::calloc(1, 72)); if (!storage) std::_Exit(88);
    auto* table = static_cast<unsigned char*>(active_loader->symbol(music, "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore22ProtocolDialogResponseENS_9allocatorIS2_EEEE"));
    *reinterpret_cast<void**>(storage) = table + 16;
    GuestShared response{storage + 24, storage};
    fn<void (*)(void*)>(store, "_ZN17storeservicescore22ProtocolDialogResponseC1Ev")(response.object);
    if (selected >= 0) {
        fn<void (*)(void*, const GuestShared*)>(store, "_ZN17storeservicescore22ProtocolDialogResponse17setSelectedButtonERKNSt6__ndk110shared_ptrINS_14ProtocolButtonEEE")(response.object, buttons->begin + selected);
        allow_credentials_prompt = true;
    } else cancelled = true;
    fn<void (*)(void*, const long*, const GuestShared*)>(music,
        "_ZN20androidstoreservices28AndroidPresentationInterface28handleProtocolDialogResponseERKlRKNSt6__ndk110shared_ptrIN17storeservicescore22ProtocolDialogResponseEEE")(presentation.object, &identifier, &response);
    fn<void (*)(void*)>("libc++_shared.so", "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(response.control);
}
}
namespace {
void requestPublicBag(Loader& loader, const GuestShared& context) {
    GuestShared request;
    callGuestReturn(loader.symbol(music,
        "_ZNSt6__ndk110shared_ptrIN17storeservicescore13URLBagRequestEE11make_sharedIJRKNS0_INS1_14RequestContextEEEEEES3_DpOT_"), &request, &context);
    if (!request.object) throw std::runtime_error("URLBagRequest factory returned empty");
    std::fputs("native-poc: requesting public URL bag\n", stderr);
    auto run = fn<void (*)(void*, double)>(store,
        "_ZN17storeservicescore13URLBagRequest14runWithTimeoutENSt6__ndk16chrono8durationIdNS1_5ratioILl1ELl1EEEEE");
    run(request.object, 15.0);
    const auto* error = fn<const GuestShared* (*)(void*)>(store, "_ZNK17storeservicescore13URLBagRequest5errorEv")(request.object);
    if (error && error->object) {
        auto* code = fn<const int* (*)(void*)>(store, "_ZNK17storeservicescore19StoreErrorCondition9errorCodeEv")(error->object);
        auto network = networkState();
        throw std::runtime_error("public URL bag failed: store=" + std::to_string(code ? *code : -1) +
            ", curl=" + std::to_string(network.result) + ", HTTP=" + std::to_string(network.http_status));
    }
    const auto* bag = fn<const GuestShared* (*)(void*)>(store, "_ZNK17storeservicescore13URLBagRequest3bagEv")(request.object);
    if (!bag || !bag->object) throw std::runtime_error("public URL bag request returned no bag");
}
}
std::string probeAuthNetwork(Loader& loader, const std::string& directory) {
    auto services = initializeServices(loader, directory, profileIdentity(directory)); active_loader = &loader;
    requestPublicBag(loader, services.context);
    GuestShared mescal;
    callGuestReturn(loader.symbol(store, "_ZN17storeservicescore14RequestContext6mescalEv"), &mescal, services.context.object);
    if (!mescal.object) throw std::runtime_error("RequestContext has no Mescal service");
    std::fputs("native-poc: establishing anonymous Mescal session\n", stderr);
    fn<void (*)(void*)>(store, "_ZN17storeservicescore6Mescal16establishSessionEv")(mescal.object);
    GuestString challenge("native-auth-preflight"); GuestShared signature;
    callGuestReturn(loader.symbol(store,
        "_ZN17storeservicescore6Mescal4signERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE"), &signature, mescal.object, &challenge);
    if (!signature.object) throw std::runtime_error("anonymous Mescal signing returned no signature");
    size_t signature_bytes = fn<size_t (*)(void*)>("libmediaplatform.so", "_ZNK13mediaplatform4Data6lengthEv")(signature.object);
    if (!signature_bytes || signature_bytes > 65536) throw std::runtime_error("anonymous Mescal signature has an invalid length");
    auto network = networkState();
    return "{\"public_url_bag\":true,\"curl_result\":" + std::to_string(network.result) +
        ",\"http_status\":" + std::to_string(network.http_status) + ",\"mescal_signature_bytes\":" + std::to_string(signature_bytes) + ",\"authenticated\":false}";
}
AuthenticationResult authenticate(Loader& loader, const std::string& directory, bool preflight, const std::string& script) {
    if (!preflight && script.empty()) throw std::runtime_error("authentication requires a native prompt script");
    setGuestLogging(false);
    auto identity = profileIdentity(directory);
    auto services = initializeServices(loader, directory, identity);
    active_loader = &loader; presentation = services.presentation;
    LoginInput input; login_input = &input; preflight_mode = preflight; prompt_script = script;
    if (!preflight) requestPublicBag(loader, services.context);
    fn<void (*)(void*, void (*)(GuestShared*, GuestShared*))>(music,
        "_ZN20androidstoreservices28AndroidPresentationInterface21setCredentialsHandlerEPFvNSt6__ndk110shared_ptrIN17storeservicescore18CredentialsRequestEEENS2_INS_33AndroidCredentialsResponseHandlerEEEE")(presentation.object, credentials);
    fn<void (*)(void*, void (*)(long, GuestShared*, GuestShared*))>(music,
        "_ZN20androidstoreservices28AndroidPresentationInterface16setDialogHandlerEPFvlNSt6__ndk110shared_ptrIN17storeservicescore14ProtocolDialogEEENS2_INS_36AndroidProtocolDialogResponseHandlerEEEE")(presentation.object, dialog);
    GuestShared flow;
    callGuestReturn(loader.symbol(music,
        "_ZNSt6__ndk110shared_ptrIN17storeservicescore16AuthenticateFlowEE11make_sharedIJRNS0_INS1_14RequestContextEEEEEES3_DpOT_"), &flow, &services.context);
    if (!flow.object || !flow.control) throw std::runtime_error("AuthenticateFlow factory returned empty");
    fn<void (*)(void*, bool)>(store, "_ZN17storeservicescore16AuthenticateFlow14setUseExistingEb")(flow.object, true);
    std::fputs("native-poc: entering real AuthenticateFlow\n", stderr);
    fn<void (*)(void*, double)>(store, "_ZN17storeservicescore16AuthenticateFlow14runWithTimeoutENSt6__ndk16chrono8durationIdNS1_5ratioILl1ELl1EEEEE")(flow.object, preflight ? 15.0 : 45.0);
    const auto* response = fn<const GuestShared* (*)(void*)>(store, "_ZNK17storeservicescore16AuthenticateFlow8responseEv")(flow.object);
    if (!response || !response->object) throw std::runtime_error("AuthenticateFlow has no response");
    int type = fn<int (*)(void*)>(store, "_ZNK17storeservicescore20AuthenticateResponse12responseTypeEv")(response->object);
    bool authenticated = type == 6;
    bool account_present = false;
    if (authenticated) {
        auto account_id = fn<int64_t (*)(void*)>(store, "_ZNK17storeservicescore20AuthenticateResponse17accountIdentifierEv")(response->object);
        GuestShared account;
        callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore14RequestContext7accountEv"), &account, services.context.object);
        account_present = account_id > 0 && account.object && account.control;
        if (!account_present) throw std::runtime_error("successful authentication response has no usable account");
    }
    const auto* error = fn<const GuestShared* (*)(void*)>(store, "_ZNK17storeservicescore20AuthenticateResponse5errorEv")(response->object);
    int error_code = 0;
    int64_t external_error = 0;
    if (error && error->object) {
        auto* code = fn<const int* (*)(void*)>(store, "_ZNK17storeservicescore19StoreErrorCondition9errorCodeEv")(error->object); if (code) error_code = *code;
        auto* external = fn<const int64_t* (*)(void*)>(store, "_ZNK17storeservicescore19StoreErrorCondition17externalErrorCodeEv")(error->object); if (external) external_error = *external;
    }
    auto network = networkState();
    login_input = nullptr;
    if (preflight && (authenticated || credential_requests != 1 || !cancelled)) throw std::runtime_error("authentication preflight did not cancel at the credential boundary");
    auto evidence = "{\"response_type\":" + std::to_string(type) + ",\"credential_requests\":" + std::to_string(credential_requests) +
        ",\"verification_requests\":" + std::to_string(verification_requests) + ",\"cancelled\":" + (cancelled ? "true" : "false") +
        ",\"protocol_dialogs\":" + std::to_string(protocol_dialogs) +
        ",\"error_code\":" + std::to_string(error_code) + ",\"account_present\":" + (account_present ? "true" : "false") +
        ",\"external_error_code\":" + std::to_string(external_error) + ",\"curl_result\":" + std::to_string(network.result) +
        ",\"http_status\":" + std::to_string(network.http_status) +
        ",\"input_error\":" + (input_error ? "true" : "false") +
        ",\"authenticated\":" + (authenticated ? "true" : "false") + "}";
    return {evidence, authenticated};
}
std::string probeAuthenticationAbi(Loader& loader, const std::string& directory) {
    auto identity = profileIdentity(directory);
    if (identity != profileIdentity(directory)) throw std::runtime_error("native profile identity changed on reopen");
    auto services = initializeServices(loader, directory, identity); (void)services;
    active_loader = &loader;
    std::string base_password(512, 'p'), code = "012345", user = "offline-fixture@example.invalid";
    if (!validVerification(code) || validVerification("12345") || validVerification("12345x")) throw std::runtime_error("verification code validation failed");
    std::string composed = base_password + code;
    auto response = credentialResponse(user, composed, true);
    const auto* stored = fn<const GuestString* (*)(void*)>(store, "_ZNK17storeservicescore19CredentialsResponse8passwordEv")(response.object);
    if (!stored || !(stored->capacity & 1) || stored->size != composed.size() || std::memcmp(stored->data, composed.data(), composed.size())) throw std::runtime_error("long password with verification code was truncated by the guest ABI");
    int type = fn<int (*)(void*)>(store, "_ZNK17storeservicescore19CredentialsResponse12responseTypeEv")(response.object);
    if (type != 2) throw std::runtime_error("credential response submit type changed");
    eraseSecretBytes(const_cast<char*>(stored->data), stored->size);
    fn<void (*)(void*)>("libc++_shared.so", "_ZNSt6__ndk119__shared_weak_count16__release_sharedEv")(response.control);
    eraseSecret(base_password); eraseSecret(composed); eraseSecret(code);
    return "{\"profile_identity_persistent\":true,\"long_password_2fa_round_trip\":true,\"verification_validation\":true,\"credentials_submitted\":false}";
}
std::string authenticationStatus(Loader& loader, const std::string& directory) {
    setGuestLogging(false);
    auto services = initializeServices(loader, directory, profileIdentity(directory));
    GuestShared account;
    callGuestReturn(loader.symbol(store, "_ZNK17storeservicescore14RequestContext7accountEv"), &account, services.context.object);
    bool present = account.object && account.control;
    return "{\"stored_account\":" + std::string(present ? "true" : "false") + ",\"credentials_revalidated\":false}";
}
}
