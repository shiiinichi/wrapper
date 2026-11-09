#include "jni_host.h"
#include "elf_loader.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace native_poc {
namespace {

struct JniTable { void** functions; };
std::array<void*, 234> environment_functions{};
std::array<void*, 8> vm_functions{};
JniTable environment{environment_functions.data()}, vm{vm_functions.data()};
size_t get_env_calls = 0;

struct Region { uintptr_t begin, end, bias; char name[80]; };
std::array<Region, 128> regions{};
size_t region_count = 0;

void raw(const char* text) { ::write(STDERR_FILENO, text, std::strlen(text)); }
void rawHex(uintptr_t value) {
    char text[2 + sizeof(value) * 2];
    text[0] = '0'; text[1] = 'x';
    for (size_t i = 0; i < sizeof(value) * 2; ++i)
        text[i + 2] = "0123456789abcdef"[(value >> ((sizeof(value) * 2 - 1 - i) * 4)) & 15];
    ::write(STDERR_FILENO, text, sizeof(text));
}
void fault(int signal, siginfo_t* info, void* context) {
    auto* state = static_cast<ucontext_t*>(context);
    uintptr_t pc = state->uc_mcontext->__ss.__pc;
    raw("native-poc: memory fault pc="); rawHex(pc);
    raw(" address="); rawHex(reinterpret_cast<uintptr_t>(info->si_addr));
    raw(" lr="); rawHex(state->uc_mcontext->__ss.__lr);
    raw(" x0="); rawHex(state->uc_mcontext->__ss.__x[0]);
    raw(" x1="); rawHex(state->uc_mcontext->__ss.__x[1]);
    raw(" x8="); rawHex(state->uc_mcontext->__ss.__x[8]);
    for (size_t i = 0; i < region_count; ++i) {
        if (pc >= regions[i].begin && pc < regions[i].end) {
            raw(" image="); raw(regions[i].name); raw(" vaddr="); rawHex(pc - regions[i].bias);
        }
    }
    raw("\n");
    _exit(128 + signal);
}

[[noreturn]] void unsupportedJni() {
    raw("native-poc: unsupported JNI function\n");
    _exit(87);
}

int getEnv(JniTable*, void** output, int version) {
    std::fprintf(stderr, "JNI GetEnv version=0x%x\n", version);
    ++get_env_calls;
    if (!output || version < 0x10001 || version > 0x10008) return -3;
    *output = &environment;
    return 0;
}

} // namespace

void installCrashDiagnostics(Loader& loader) {
    region_count = 0;
    for (const auto& image : loader.images()) {
        if (region_count == regions.size()) throw std::runtime_error("too many diagnostic regions");
        auto& r = regions[region_count++];
        r.begin = image->mappingBegin(); r.end = image->mappingEnd(); r.bias = image->loadBias();
        std::snprintf(r.name, sizeof(r.name), "%s", image->report.library.c_str());
    }
    struct sigaction action{};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr); sigaction(SIGBUS, &action, nullptr);
}

std::string probeAppleJni(Loader& loader, const std::string& library) {
    loader.load(library);
    loader.relocate();
    installCrashDiagnostics(loader);
    loader.prepareExecution();
    loader.initialize();
    environment_functions.fill(reinterpret_cast<void*>(&unsupportedJni));
    vm_functions.fill(reinterpret_cast<void*>(&unsupportedJni));
    vm_functions[6] = reinterpret_cast<void*>(&getEnv);
    void* entry = loader.symbol(library, "JNI_OnLoad");
    bool valid = false;
    for (const auto& image : loader.images())
        if (image->report.library == library && image->executableAddress(reinterpret_cast<uintptr_t>(entry))) valid = true;
    if (!valid) throw std::runtime_error("JNI_OnLoad is outside the library's executable segments");
    auto onLoad = reinterpret_cast<int (*)(JniTable*, void*)>(entry);
    std::fprintf(stderr, "JNI entering %s\n", library.c_str());
    int version = onLoad(&vm, nullptr);
    if (version < 0x10001 || version > 0x10008) throw std::runtime_error("JNI_OnLoad returned an unsupported JNI version");
    return "{\"jni_version\":" + std::to_string(version) + ",\"get_env_calls\":" +
        std::to_string(get_env_calls) + ",\"registered_natives\":[]}";
}

} // namespace native_poc
