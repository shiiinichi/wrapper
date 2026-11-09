#include "sqlite_bridge.h"
#include "posix_bridge.h"
#include "abi_runtime.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sqlite3.h>
#include <unistd.h>
namespace native_poc {
namespace {
std::atomic<size_t> opened{0};
bool allowed(const char* path) { return path && (!std::strcmp(path, ":memory:") || writablePath(path)); }
int openDatabase(const char* path, sqlite3** output) {
    if (!allowed(path)) { if (output) *output = nullptr; return SQLITE_CANTOPEN; }
    int code = sqlite3_open(path, output); if (!code) ++opened; return code;
}
int openDatabaseV2(const char* path, sqlite3** output, int flags, const char* vfs) {
    // Keep database writes in the same dedicated profile as guest POSIX writes.
    if (!allowed(path) || vfs) { if (output) *output = nullptr; return SQLITE_CANTOPEN; }
    int code = sqlite3_open_v2(path, output, flags, vfs); if (!code) ++opened; return code;
}
[[noreturn]] void unsupportedOption(int option) { std::fprintf(stderr, "native-poc: unsupported SQLite configuration %d\n", option); _exit(86); }
int configuration(int option, uintptr_t a, uintptr_t b) {
    switch (option) {
        case SQLITE_CONFIG_SINGLETHREAD: case SQLITE_CONFIG_MULTITHREAD: case SQLITE_CONFIG_SERIALIZED: return sqlite3_config(option);
        case SQLITE_CONFIG_MEMSTATUS: case SQLITE_CONFIG_URI: return sqlite3_config(option, static_cast<int>(a));
        case SQLITE_CONFIG_LOG: return sqlite3_config(option, reinterpret_cast<void (*)(void*, int, const char*)>(a), reinterpret_cast<void*>(b));
        default: unsupportedOption(option);
    }
}
int databaseConfiguration(sqlite3* database, int option, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (option == SQLITE_DBCONFIG_LOOKASIDE) return sqlite3_db_config(database, option, reinterpret_cast<void*>(a), static_cast<int>(b), static_cast<int>(c));
    if (option == SQLITE_DBCONFIG_MAINDBNAME) return sqlite3_db_config(database, option, reinterpret_cast<const char*>(a));
    if (option == SQLITE_DBCONFIG_ENABLE_FKEY || option == SQLITE_DBCONFIG_ENABLE_TRIGGER)
        return sqlite3_db_config(database, option, static_cast<int>(a), reinterpret_cast<int*>(b));
    unsupportedOption(option);
}
}
size_t nativeSqliteOpenCount() { return opened; }
Bridge sqliteBridge(const std::string& name, bool data) {
    if (name.rfind("sqlite3_", 0) != 0) return {};
    if (name == "sqlite3_open") return {reinterpret_cast<void*>(&openDatabase), false, false};
    if (name == "sqlite3_open_v2") return {reinterpret_cast<void*>(&openDatabaseV2), false, false};
    if (name == "sqlite3_config") return {reinterpret_cast<void*>(&configuration), false, false};
    if (name == "sqlite3_db_config") return {reinterpret_cast<void*>(&databaseConfiguration), false, false};
    // Native SQLite owns its opaque handles. Never pass them to Android SQLite
    // fallback code. Varargs outside the reviewed options require adapters.
    const std::string variadic = "|sqlite3_mprintf|sqlite3_vmprintf|sqlite3_snprintf|sqlite3_vsnprintf|sqlite3_test_control|sqlite3_vtab_config|sqlite3_open16|";
    if (!data && variadic.find("|" + name + "|") != std::string::npos) return {guardedImport(name), true, false};
    static void* library = dlopen("/usr/lib/libsqlite3.dylib", RTLD_NOW | RTLD_LOCAL);
    void* address = library ? dlsym(library, name.c_str()) : nullptr;
    if (address) return {address, false, data};
    if (!data) return {guardedImport(name), true, false};
    // Private Android-only debug data remains with its unused implementation.
    return {};
}
}
