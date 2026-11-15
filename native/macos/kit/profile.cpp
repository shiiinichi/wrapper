#include "runtime.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wrapper {
namespace fs = std::filesystem;
namespace {
struct Descriptor {
    int fd = -1;
    explicit Descriptor(int fd) : fd(fd) {}
    ~Descriptor() {
        if (fd >= 0)
            ::close(fd);
    }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};
bool under(const fs::path &child, const fs::path &parent) {
    auto relative = child.lexically_relative(parent);
    return !relative.empty() && *relative.begin() != "..";
}
bool singleValue(sqlite3 *database, const char *sql, const char *expected) {
    sqlite3_stmt *raw = nullptr;
    int code = sqlite3_prepare_v2(database, sql, -1, &raw, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    if (code != SQLITE_OK || sqlite3_step(raw) != SQLITE_ROW)
        return false;
    auto value = sqlite3_column_text(raw, 0);
    return value && std::strcmp(reinterpret_cast<const char *>(value), expected) == 0 &&
           sqlite3_step(raw) == SQLITE_DONE;
}
void validateDatabase(const fs::path &path, bool account) {
    sqlite3 *raw = nullptr;
    int code = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (code != SQLITE_OK || !singleValue(raw, "PRAGMA quick_check", "ok"))
        fail(NMW_MESSAGE, "The imported account contains an invalid database.");
    if (account && !singleValue(raw,
            "SELECT 1 FROM account WHERE active = 1 AND dsid > 0 "
            "AND typeof(id) = 'text' AND length(id) > 0 LIMIT 1", "1"))
        fail(NMW_MESSAGE, "The imported profile has no usable saved account.");
}
bool copyKnown(int folder, const char *name, const fs::path &target, size_t &total) {
    Descriptor source(openat(folder, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (source.fd < 0 && errno == ENOENT)
        return false;
    if (source.fd < 0)
        fail(NMW_MESSAGE, "A native account file could not be opened. Symbolic links are not supported.");
    struct stat s{};
    if (fstat(source.fd, &s) || !S_ISREG(s.st_mode) || s.st_uid != getuid() || s.st_size < 0 ||
        s.st_size > 64 * 1024 * 1024 || (!std::strcmp(name, "music-client-v1") && s.st_size != 0))
        fail(NMW_MESSAGE, "Invalid native account file.");
    total += static_cast<size_t>(s.st_size);
    if (total > 128 * 1024 * 1024)
        fail(NMW_OVERSIZED);
    Descriptor dest(open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (dest.fd < 0)
        fail(NMW_UNAVAILABLE);
    std::array<uint8_t, 65536> buffer{};
    size_t remaining = static_cast<size_t>(s.st_size);
    while (remaining) {
        auto got = read(source.fd, buffer.data(), std::min(remaining, buffer.size()));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            fail(NMW_INVALID);
        size_t sent = 0;
        while (sent < static_cast<size_t>(got)) {
            auto n = write(dest.fd, buffer.data() + sent, static_cast<size_t>(got) - sent);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                fail(NMW_UNAVAILABLE);
            sent += static_cast<size_t>(n);
        }
        remaining -= static_cast<size_t>(got);
    }
    return true;
}
} // namespace
void copyProfile(const std::string &from, const std::string &to) {
    if (from.empty() || to.empty())
        fail(NMW_INVALID);
    const auto source = fs::absolute(from).lexically_normal(), dest = fs::absolute(to).lexically_normal();
    if (fs::is_symlink(fs::symlink_status(source)) ||
        under(fs::weakly_canonical(source), fs::weakly_canonical(dest)) ||
        under(fs::weakly_canonical(dest), fs::weakly_canonical(source)))
        fail(NMW_MESSAGE, "Choose a distinct wrapper account folder.");
    Descriptor folder(open(source.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (folder.fd < 0)
        fail(NMW_MESSAGE, "The source account folder could not be opened.");
    Descriptor lock(openat(folder.fd, "native-api.lock", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (lock.fd < 0 && errno != ENOENT)
        fail(NMW_UNAVAILABLE);
    if (lock.fd >= 0 && flock(lock.fd, LOCK_EX | LOCK_NB))
        fail(NMW_MESSAGE, "Quit Native Music Wrapper before importing its account.");
    fs::create_directories(dest.parent_path());
    auto pattern = (dest.parent_path() / ".native-import-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
        fail(NMW_UNAVAILABLE);
    fs::path stage(pattern);
    struct Cleanup {
        fs::path p;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } cleanup{stage};
    size_t bytes = 0;
    auto idPath = stage / "native-device-id";
    bool native = copyKnown(folder.fd, "native-device-id", idPath, bytes);
    bool android = !native && copyKnown(folder.fd, "ANDROID_ID", idPath, bytes);
    bool marked = copyKnown(folder.fd, "music-client-v1", stage / "music-client-v1", bytes);
    // All imported profiles use the canonical Android SDK layout. Only the
    // location of the source database differs for older flat data folders.
    Descriptor nested(
        (android || marked) ? openat(folder.fd, "mpl_db", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1);
    if ((android || marked) && nested.fd < 0)
        fail(NMW_MESSAGE, "The wrapper account database folder could not be opened.");
    auto database = stage / "mpl_db";
    fs::create_directory(database);
    chmod(database.c_str(), 0700);
    for (auto name : {"accounts", "cookies", "httpcache", "kvs"})
        for (auto suffix : {".sqlitedb", ".sqlitedb-wal", ".sqlitedb-shm"}) {
            auto file = std::string(name) + suffix;
            copyKnown(nested.fd >= 0 ? nested.fd : folder.fd, file.c_str(), database / file, bytes);
        }
    for (auto name : {"adi.pb", "IC-Info.sids", "fsi.pdat"})
        copyKnown(folder.fd, name, stage / name, bytes);
    Descriptor id(open(idPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    char buf[17];
    if (id.fd < 0 || read(id.fd, buf, sizeof(buf)) != 16 ||
        !std::all_of(
            buf, buf + 16, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
        !fs::is_regular_file(database / "accounts.sqlitedb"))
        fail(NMW_MESSAGE, "This folder does not contain a native wrapper account.");
    for (auto name : {"accounts", "cookies", "httpcache", "kvs"}) {
        auto path = database / (std::string(name) + ".sqlitedb");
        if (fs::exists(path))
            validateDatabase(path, std::strcmp(name, "accounts") == 0);
    }
    if (!marked) {
        Descriptor mark(open((stage / "music-client-v1").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
        if (mark.fd < 0)
            fail(NMW_UNAVAILABLE);
    }
    fs::create_directory(dest);
    Descriptor destination(open(dest.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (destination.fd < 0)
        fail(NMW_UNAVAILABLE);
    Descriptor destinationLock(openat(destination.fd, "native-api.lock",
                                      O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    struct stat locked{}, directory{}, current{};
    if (destinationLock.fd < 0 || fstat(destinationLock.fd, &locked) || !S_ISREG(locked.st_mode) ||
        locked.st_uid != getuid())
        fail(NMW_UNAVAILABLE);
    if (flock(destinationLock.fd, LOCK_EX | LOCK_NB))
        fail(NMW_BUSY);
    // A concurrent import may have replaced the directory while we opened it.
    if (fstat(destination.fd, &directory) || lstat(dest.c_str(), &current) ||
        directory.st_dev != current.st_dev || directory.st_ino != current.st_ino)
        fail(NMW_BUSY);
    // Keep the same lock inode at the published path across the atomic swap.
    // Workers opening either directory observe this lock until import finishes.
    if (linkat(destination.fd, "native-api.lock", AT_FDCWD, (stage / "native-api.lock").c_str(), 0) ||
        lstat((stage / "native-api.lock").c_str(), &current) ||
        locked.st_dev != current.st_dev || locked.st_ino != current.st_ino)
        fail(NMW_UNAVAILABLE);
    if (renameatx_np(AT_FDCWD, stage.c_str(), AT_FDCWD, dest.c_str(), RENAME_SWAP))
        fail(NMW_UNAVAILABLE);
}
} // namespace wrapper
