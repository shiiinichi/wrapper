#include "network_bridge.h"
#include <curl/curl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <atomic>
namespace native_poc {
namespace {
std::atomic<unsigned> transfers{0};
std::atomic<int> last_result{0};
std::atomic<long> last_status{0};
CURLcode perform(CURL* handle) {
    auto code = curl_easy_perform(handle); ++transfers; last_result = code;
    long status = 0; curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status); last_status = status;
    std::fprintf(stderr, "native-poc: native curl transfer completed code=%d\n", static_cast<int>(code));
    return code;
}
[[noreturn]] void unsupported(const char* api, int option) {
    std::fprintf(stderr, "native-poc: unsupported %s option %d\n", api, option); _exit(86);
}
// Linux puts the first variadic value in x2. A fixed-argument entry receives it,
// then a typed native call emits the Darwin variadic stack slot.
CURLcode easyOption(CURL* handle, int value, uintptr_t argument) {
    auto option = static_cast<CURLoption>(value);
    if (option == CURLOPT_SSL_VERIFYPEER) argument = 1;
    if (option == CURLOPT_SSL_VERIFYHOST) argument = 2;
    if (option == CURLOPT_VERBOSE) argument = 0;
    if (option == CURLOPT_CAINFO && argument && std::strncmp(reinterpret_cast<const char*>(argument), "/system/", 8) == 0)
        argument = reinterpret_cast<uintptr_t>("/etc/ssl/cert.pem");
    if (option == CURLOPT_OPENSOCKETFUNCTION || option == CURLOPT_SSL_CTX_FUNCTION) unsupported("curl_easy_setopt callback ABI", value);
    if (value >= 0 && value < 10000) return curl_easy_setopt(handle, option, static_cast<long>(argument));
    if (value < 20000) return curl_easy_setopt(handle, option, reinterpret_cast<void*>(argument));
    if (value < 30000) return curl_easy_setopt(handle, option, reinterpret_cast<void (*)()>(argument));
    if (value < 40000) return curl_easy_setopt(handle, option, static_cast<curl_off_t>(argument));
    unsupported("curl_easy_setopt", value);
}
CURLcode easyInfo(CURL* handle, int value, void* pointer) {
    auto info = static_cast<CURLINFO>(value);
    switch (value & CURLINFO_TYPEMASK) {
        case CURLINFO_STRING: return curl_easy_getinfo(handle, info, static_cast<char**>(pointer));
        case CURLINFO_LONG: return curl_easy_getinfo(handle, info, static_cast<long*>(pointer));
        case CURLINFO_DOUBLE: return curl_easy_getinfo(handle, info, static_cast<double*>(pointer));
        case CURLINFO_SLIST: return curl_easy_getinfo(handle, info, static_cast<curl_slist**>(pointer));
        case CURLINFO_SOCKET: return curl_easy_getinfo(handle, info, static_cast<curl_socket_t*>(pointer));
        case CURLINFO_OFF_T: return curl_easy_getinfo(handle, info, static_cast<curl_off_t*>(pointer));
        default: unsupported("curl_easy_getinfo", value);
    }
}
CURLSHcode shareOption(CURLSH* handle, int value, uintptr_t argument) {
    auto option = static_cast<CURLSHoption>(value);
    switch (option) {
        case CURLSHOPT_SHARE: case CURLSHOPT_UNSHARE: return curl_share_setopt(handle, option, static_cast<int>(argument));
        case CURLSHOPT_LOCKFUNC: return curl_share_setopt(handle, option, reinterpret_cast<curl_lock_function>(argument));
        case CURLSHOPT_UNLOCKFUNC: return curl_share_setopt(handle, option, reinterpret_cast<curl_unlock_function>(argument));
        case CURLSHOPT_USERDATA: return curl_share_setopt(handle, option, reinterpret_cast<void*>(argument));
        default: unsupported("curl_share_setopt", value);
    }
}
CURLMcode multiOption(CURLM* handle, int value, uintptr_t argument) {
    auto option = static_cast<CURLMoption>(value);
    if (value >= 0 && value < 10000) return curl_multi_setopt(handle, option, static_cast<long>(argument));
    if (value < 20000) return curl_multi_setopt(handle, option, reinterpret_cast<void*>(argument));
    if (option == CURLMOPT_SOCKETFUNCTION) return curl_multi_setopt(handle, option, reinterpret_cast<curl_socket_callback>(argument));
    if (option == CURLMOPT_TIMERFUNCTION) return curl_multi_setopt(handle, option, reinterpret_cast<curl_multi_timer_callback>(argument));
    unsupported("curl_multi_setopt", value);
}
}
NetworkState networkState() { return {transfers.load(), last_result.load(), last_status.load()}; }
Bridge networkBridge(const std::string& name) {
#define BRIDGE(symbol, function) if (name == symbol) return {reinterpret_cast<void*>(&function), false, false}
    BRIDGE("curl_easy_setopt", easyOption); BRIDGE("curl_easy_getinfo", easyInfo);
    BRIDGE("curl_share_setopt", shareOption); BRIDGE("curl_multi_setopt", multiOption);
    BRIDGE("curl_easy_perform", perform);
#undef BRIDGE
    return {};
}
}
