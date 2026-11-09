#include "posix_bridge.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>

namespace native_poc {
namespace {
std::filesystem::path write_root;
std::mutex descriptor_lock;
std::set<int> writable_descriptors;
template<class T> T result(T value) { if (value == T(-1)) guestErrno() = linuxError(errno); return value; }
[[noreturn]] void unsupported(const char* operation) {
    std::fprintf(stderr, "native-poc: unsupported Android POSIX operation: %s\n", operation); _exit(86);
}
bool writableFd(int fd) { std::lock_guard<std::mutex> lock(descriptor_lock); return writable_descriptors.count(fd) || fd == 1 || fd == 2; }
int nativeFlags(int flags) {
    int native = flags & 3;
    const int linux[] = {0100, 0200, 0400, 01000, 02000, 04000, 010000, 0200000, 0400000, 02000000, 04000000};
    const int host[] = {O_CREAT, O_EXCL, O_NOCTTY, O_TRUNC, O_APPEND, O_NONBLOCK, O_DSYNC, O_DIRECTORY, O_NOFOLLOW, O_CLOEXEC, O_SYNC};
    int known = 3 | 0100000; // O_LARGEFILE is a no-op on LP64.
    for (size_t i = 0; i < sizeof(linux)/sizeof(linux[0]); ++i) { known |= linux[i]; if (flags & linux[i]) native |= host[i]; }
    if (flags & ~known) unsupported("open flags");
    return native;
}
int openBridge(const char* path, int flags, unsigned mode) {
    bool writing = flags & (3 | 0100 | 01000);
    if (writing && !writablePath(path)) { guestErrno() = 13; return -1; }
    int fd = result(::open(path, nativeFlags(flags), static_cast<mode_t>(mode & 0777)));
    if (fd >= 0 && writing) { std::lock_guard<std::mutex> lock(descriptor_lock); writable_descriptors.insert(fd); }
    return fd;
}
int openChecked(const char* path, int flags) {
    if (flags & 0100) unsupported("__open_2 requires a creation mode");
    return openBridge(path, flags, 0);
}
int closeBridge(int fd) {
    int value = result(::close(fd));
    if (!value) { std::lock_guard<std::mutex> lock(descriptor_lock); writable_descriptors.erase(fd); }
    return value;
}
ssize_t readBridge(int fd, void* data, size_t size) { return result(::read(fd, data, size)); }
ssize_t writeBridge(int fd, const void* data, size_t size) { if (!writableFd(fd)) { guestErrno() = 13; return -1; } return result(::write(fd, data, size)); }
ssize_t preadBridge(int fd, void* data, size_t size, int64_t offset) { return result(::pread(fd, data, size, offset)); }
ssize_t pwriteBridge(int fd, const void* data, size_t size, int64_t offset) { if (!writableFd(fd)) { guestErrno() = 13; return -1; } return result(::pwrite(fd, data, size, offset)); }
int64_t lseekBridge(int fd, int64_t offset, int origin) { return result(::lseek(fd, offset, origin)); }
int truncateBridge(int fd, int64_t size) { if (!writableFd(fd)) { guestErrno() = 13; return -1; } return result(::ftruncate(fd, size)); }
int mkdirBridge(const char* path, unsigned mode) { if (!writablePath(path)) { guestErrno() = 13; return -1; } return result(::mkdir(path, mode & 0777)); }
int chmodBridge(const char* path, unsigned mode) { if (!writablePath(path)) { guestErrno() = 13; return -1; } return result(::chmod(path, mode & 0777)); }
int unlinkBridge(const char* path) { if (!writablePath(path)) { guestErrno() = 13; return -1; } return result(::unlink(path)); }
int renameBridge(const char* old, const char* path) { if (!writablePath(old) || !writablePath(path)) { guestErrno() = 13; return -1; } return result(::rename(old, path)); }
int fsyncBridge(int fd) { return result(::fsync(fd)); }
int accessBridge(const char* path, int mode) { return result(::access(path, mode)); }
int flockBridge(int fd, int operation) { return result(::flock(fd, operation)); }

struct GuestStat {
    uint64_t device, inode;
    uint32_t mode, links, uid, gid;
    uint64_t rdevice, pad;
    int64_t size;
    int32_t block_size, pad2;
    int64_t blocks;
    timespec access, modification, change;
    uint32_t unused[2];
};
static_assert(sizeof(GuestStat) == 128 && offsetof(GuestStat, size) == 48 && offsetof(GuestStat, access) == 72);
void translateStat(GuestStat* output, const struct stat& value) {
    *output = {}; output->device = value.st_dev; output->inode = value.st_ino; output->mode = value.st_mode;
    output->links = value.st_nlink; output->uid = value.st_uid; output->gid = value.st_gid; output->rdevice = value.st_rdev;
    output->size = value.st_size; output->block_size = value.st_blksize; output->blocks = value.st_blocks;
    output->access = value.st_atimespec; output->modification = value.st_mtimespec; output->change = value.st_ctimespec;
}
int statBridge(const char* path, GuestStat* output) { struct stat value{}; int code = result(::stat(path, &value)); if (!code) translateStat(output, value); return code; }
int lstatBridge(const char* path, GuestStat* output) { struct stat value{}; int code = result(::lstat(path, &value)); if (!code) translateStat(output, value); return code; }
int fstatBridge(int fd, GuestStat* output) { struct stat value{}; int code = result(::fstat(fd, &value)); if (!code) translateStat(output, value); return code; }
struct GuestFlock { int16_t type, whence; int32_t pad; int64_t start, length; int32_t pid, pad2; };
int fcntlBridge(int fd, int command, uintptr_t argument) {
    if (command == 1 || command == 2) return result(::fcntl(fd, command == 1 ? F_GETFD : F_SETFD, static_cast<int>(argument)));
    if (command >= 5 && command <= 7) {
        auto* input = reinterpret_cast<GuestFlock*>(argument);
        if (input->type < 0 || input->type > 2) { guestErrno() = 22; return -1; }
        struct flock native{}; native.l_type = input->type == 0 ? F_RDLCK : input->type == 1 ? F_WRLCK : F_UNLCK;
        native.l_whence = input->whence; native.l_start = input->start; native.l_len = input->length; native.l_pid = input->pid;
        int code = result(::fcntl(fd, command == 5 ? F_GETLK : command == 6 ? F_SETLK : F_SETLKW, &native));
        if (!code && command == 5) { input->type = native.l_type == F_RDLCK ? 0 : native.l_type == F_WRLCK ? 1 : 2; input->whence = native.l_whence; input->start = native.l_start; input->length = native.l_len; input->pid = native.l_pid; }
        return code;
    }
    unsupported("fcntl command");
}
int gettimeofdayBridge(int64_t* output, void* zone) {
    if (zone) unsupported("gettimeofday timezone");
    timeval value{}; int code = result(::gettimeofday(&value, nullptr));
    if (!code && output) { output[0] = value.tv_sec; output[1] = value.tv_usec; } return code;
}
int clockBridge(int clock, timespec* output) {
    const clockid_t clocks[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, CLOCK_MONOTONIC_RAW};
    if (clock < 0 || clock > 4) { guestErrno() = 22; return -1; }
    return result(::clock_gettime(clocks[clock], output));
}
int nanosleepBridge(const timespec* duration, timespec* remaining) { return result(::nanosleep(duration, remaining)); }
long sysconfBridge(int name) {
    int native;
    switch (name) {
        case 0: native = _SC_ARG_MAX; break;
        case 5: native = _SC_CHILD_MAX; break;
        case 6: native = _SC_CLK_TCK; break;
        case 11: native = _SC_OPEN_MAX; break;
        case 0x27: case 0x28: native = _SC_PAGESIZE; break;
        case 0x4b: native = _SC_THREAD_KEYS_MAX; break;
        case 0x4c: native = _SC_THREAD_STACK_MIN; break;
        case 0x60: native = _SC_NPROCESSORS_CONF; break;
        case 0x61: native = _SC_NPROCESSORS_ONLN; break;
        default: guestErrno() = 22; return -1;
    }
    return result(::sysconf(native));
}
char* getcwdBridge(char* output, size_t capacity) { auto* value = ::getcwd(output, capacity); if (!value) guestErrno() = linuxError(errno); return value; }
void* mmapBridge(void* address, size_t size, int protection, int flags, int fd, int64_t offset) {
    if ((protection & ~3) || (flags & ~(1 | 2 | 0x20)) || (flags & 3) == 3) unsupported("mmap protection/flags");
    if ((flags & 1) && (protection & 2) && !writableFd(fd)) { guestErrno() = 13; return MAP_FAILED; }
    int native = (flags & 1 ? MAP_SHARED : MAP_PRIVATE) | (flags & 0x20 ? MAP_ANON : 0);
    void* value = ::mmap(address, size, protection, native, fd, offset);
    if (value == MAP_FAILED) guestErrno() = linuxError(errno); return value;
}
int munmapBridge(void* address, size_t size) { return result(::munmap(address, size)); }
int socketBridge(int domain, int type, int protocol) {
    int host_domain = domain == 2 ? AF_INET : domain == 10 ? AF_INET6 : domain == 1 ? AF_UNIX : -1;
    if (host_domain < 0 || (type & ~(0x80000 | 0x800 | 0xf))) unsupported("socket domain/type");
    int fd = result(::socket(host_domain, type & 0xf, protocol));
    if (fd >= 0) {
        if (type & 0x80000) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (type & 0x800) ::fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    return fd;
}
int ioctlBridge(int fd, unsigned long command, void* argument) {
    // Linux and Darwin ifreq/sockaddr/command values differ. Only the queries
    // used by the device identity path are implemented; no native ioctl pass-through.
    if (command != 0x8912 && command != 0x8913 && command != 0x8927) {
        std::fprintf(stderr, "native-poc: unsupported Linux ioctl 0x%lx\n", command); _exit(86);
    }
    struct ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces)) return result(-1);
    if (command == 0x8912) {
        struct Configuration { int32_t length, pad; unsigned char* requests; };
        auto* config = static_cast<Configuration*>(argument);
        int capacity = config->length, used = 0;
        for (auto* item = interfaces; item; item = item->ifa_next) {
            if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET) continue;
            if (config->requests && used + 40 > capacity) break;
            if (config->requests) {
                auto* output = config->requests + used; std::memset(output, 0, 40);
                std::strncpy(reinterpret_cast<char*>(output), item->ifa_name, 15);
                std::memcpy(output + 16, item->ifa_addr, 16); output[16] = 2; output[17] = 0;
            }
            used += 40;
        }
        config->length = used; ::freeifaddrs(interfaces); return 0;
    }
    auto* request = static_cast<unsigned char*>(argument);
    for (auto* item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_addr || std::strncmp(item->ifa_name, reinterpret_cast<char*>(request), 16)) continue;
        if (command == 0x8913) {
            uint16_t flags = (item->ifa_flags & 0x3ff) | ((item->ifa_flags & IFF_MULTICAST) ? 0x1000 : 0);
            std::memcpy(request + 16, &flags, 2); ::freeifaddrs(interfaces); return 0;
        }
        if (item->ifa_addr->sa_family != AF_LINK) continue;
        auto* link = reinterpret_cast<sockaddr_dl*>(item->ifa_addr);
        if (link->sdl_alen != 6) continue;
        std::memset(request + 16, 0, 24); request[16] = 1;
        std::memcpy(request + 18, LLADDR(link), 6); ::freeifaddrs(interfaces); return 0;
    }
    ::freeifaddrs(interfaces); guestErrno() = 19; (void)fd; return -1;
}
} // namespace

void setRuntimeDirectory(const std::string& path) { write_root = std::filesystem::canonical(path); }
bool writablePath(const char* path) {
    if (write_root.empty() || !path) return false;
    std::error_code error; auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    auto relative = resolved.lexically_relative(write_root);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& component : relative) if (component == "..") return false;
    return true;
}
Bridge posixBridge(const std::string& name) {
#define BRIDGE(symbol, function) if (name == symbol) return {reinterpret_cast<void*>(&function), false, false}
    BRIDGE("open", openBridge); BRIDGE("open64", openBridge); BRIDGE("close", closeBridge);
    BRIDGE("__open_2", openChecked);
    BRIDGE("read", readBridge); BRIDGE("write", writeBridge); BRIDGE("pread", preadBridge); BRIDGE("pread64", preadBridge);
    BRIDGE("pwrite", pwriteBridge); BRIDGE("pwrite64", pwriteBridge); BRIDGE("lseek", lseekBridge); BRIDGE("lseek64", lseekBridge);
    BRIDGE("ftruncate", truncateBridge); BRIDGE("ftruncate64", truncateBridge); BRIDGE("mkdir", mkdirBridge); BRIDGE("chmod", chmodBridge);
    BRIDGE("unlink", unlinkBridge); BRIDGE("rename", renameBridge); BRIDGE("fsync", fsyncBridge); BRIDGE("fdatasync", fsyncBridge);
    BRIDGE("stat", statBridge); BRIDGE("stat64", statBridge); BRIDGE("lstat", lstatBridge); BRIDGE("lstat64", lstatBridge);
    BRIDGE("fstat", fstatBridge); BRIDGE("fstat64", fstatBridge); BRIDGE("fcntl", fcntlBridge); BRIDGE("flock", flockBridge); BRIDGE("access", accessBridge);
    BRIDGE("gettimeofday", gettimeofdayBridge); BRIDGE("clock_gettime", clockBridge); BRIDGE("nanosleep", nanosleepBridge);
    BRIDGE("socket", socketBridge); BRIDGE("ioctl", ioctlBridge);
    BRIDGE("sysconf", sysconfBridge); BRIDGE("getcwd", getcwdBridge); BRIDGE("mmap", mmapBridge); BRIDGE("mmap64", mmapBridge); BRIDGE("munmap", munmapBridge);
#undef BRIDGE
    return {};
}
} // namespace native_poc
