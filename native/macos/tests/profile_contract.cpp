#include "test_support.h"
#include <sqlite3.h>
#include <sys/file.h>
#include <sys/stat.h>
using namespace validation;
namespace {
struct Database {
    sqlite3 *value = nullptr;
    explicit Database(const fs::path &path) {
        require(sqlite3_open(path.c_str(), &value) == SQLITE_OK, "fixture database");
    }
    ~Database() { sqlite3_close(value); }
    void execute(const char *sql) {
        require(sqlite3_exec(value, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "fixture SQL");
    }
};
void account(Database &database) {
    database.execute("CREATE TABLE account(active INTEGER, dsid INTEGER, id TEXT);"
                     "INSERT INTO account VALUES(1, 42, 'fixture@example.invalid')");
}
}
void profileTests(Report &report) {
    report.run("profile import respects the destination lock and preserves its inode", [] {
        Temp t;
        auto source = t.path / "source", dest = t.path / "dest";
        fs::create_directory(source);
        fs::create_directory(dest);
        write(source / "native-device-id", "0123456789abcdef");
        { Database db(source / "accounts.sqlitedb"); account(db); }
        write(dest / "keep", "existing");
        int fd = open((dest / "native-api.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        require(fd >= 0, "destination lock");
        struct Close { int fd; ~Close() { close(fd); } } held{fd};
        require(!flock(fd, LOCK_EX | LOCK_NB), "lock destination");
        struct stat before{}, after{};
        require(!fstat(fd, &before), "lock identity");
        require(nmw_import_profile(source.c_str(), dest.c_str()) == NMW_BUSY &&
                    read(dest / "keep") == "existing", "active destination replaced");
        require(!flock(fd, LOCK_UN), "unlock destination");
        require(nmw_import_profile(source.c_str(), dest.c_str()) == NMW_OK, "stopped destination rejected");
        require(!stat((dest / "native-api.lock").c_str(), &after) &&
                    before.st_ino == after.st_ino && before.st_dev == after.st_dev, "lock inode replaced");
        require(!flock(fd, LOCK_EX | LOCK_NB), "relock published profile");
        require(nmw_import_profile(source.c_str(), dest.c_str()) == NMW_BUSY, "old descriptor lost exclusion");
    });
    report.run("invalid account imports preserve the complete destination", [] {
        for (auto invalid : {"corrupt", "empty", "signed out", "corrupt cookies"}) {
            Temp t;
            auto source = t.path / "source", dest = t.path / "dest";
            fs::create_directory(source);
            fs::create_directory(dest);
            write(source / "native-device-id", "0123456789abcdef");
            write(dest / "keep", "existing profile");
            if (std::string(invalid) == "corrupt")
                write(source / "accounts.sqlitedb", "not a database");
            else {
                Database db(source / "accounts.sqlitedb");
                if (std::string(invalid) != "empty") account(db);
                if (std::string(invalid) == "signed out") db.execute("UPDATE account SET active = 0");
                if (std::string(invalid) == "corrupt cookies") write(source / "cookies.sqlitedb", "bad");
            }
            require(nmw_import_profile(source.c_str(), dest.c_str()) != NMW_OK, "invalid import succeeded");
            require(read(dest / "keep") == "existing profile" &&
                        !fs::exists(dest / "mpl_db"), "previous profile replaced");
        }
    });
    report.run("flat Android and marked native imports preserve usable WAL accounts", [] {
        for (auto shape : {"flat", "Android", "marked"}) {
            Temp t;
            auto source = t.path / "source", dest = t.path / "dest";
            fs::create_directory(source);
            bool android = std::string(shape) == "Android", flat = std::string(shape) == "flat";
            write(source / (android ? "ANDROID_ID" : "native-device-id"), "0123456789abcdef");
            if (std::string(shape) == "marked") write(source / "music-client-v1", "");
            auto folder = flat ? source : source / "mpl_db";
            fs::create_directories(folder);
            Database db(folder / "accounts.sqlitedb");
            db.execute("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
            account(db);
            require(nmw_import_profile(source.c_str(), dest.c_str()) == NMW_OK, "usable import rejected");
            Database imported(dest / "mpl_db" / "accounts.sqlitedb");
            imported.execute("CREATE TABLE verified AS SELECT * FROM account WHERE active = 1");
            require(read(dest / "native-device-id") == "0123456789abcdef" &&
                        fs::exists(dest / "music-client-v1"), "identity or layout changed");
        }
    });
}
