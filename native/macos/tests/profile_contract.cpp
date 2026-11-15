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
    database.execute(
        "CREATE TABLE account(active INTEGER DEFAULT 0, creditString TEXT, dsid INTEGER PRIMARY KEY, "
        "firstName TEXT, id TEXT, im_can_subscribe INTEGER DEFAULT -1, im_subscribed INTEGER DEFAULT -1, "
        "im_supported INTEGER DEFAULT -1, lastName TEXT, storeFront TEXT, accountFlags TEXT);"
        "CREATE TABLE profile(entityId INTEGER PRIMARY KEY, entityType TEXT, name TEXT, handle TEXT, bio TEXT, "
        "profileImage TEXT, backgroundImage TEXT, isVerified INTEGER DEFAULT 0, isFollowable INTEGER DEFAULT 0, "
        "autoFollowEnable INTEGER DEFAULT 0, isPrivate INTEGER NOT NULL DEFAULT 0, "
        "isOnBoarded INTEGER NOT NULL DEFAULT 0, isDiscoverableByContact INTEGER NOT NULL DEFAULT 0, "
        "socialProfileId TEXT NOT NULL DEFAULT '', isOnboardingBlocked INTEGER NOT NULL DEFAULT 0, "
        "isContactsCheckAllowed INTEGER NOT NULL DEFAULT 0, discoverabilityConsentVersion INTEGER NOT NULL DEFAULT 0);"
        "PRAGMA user_version=4;"
        "INSERT INTO account(active,dsid,id,firstName,storeFront) "
        "VALUES(1,42,'fixture@example.invalid','Fixture','143441-1,31')");
}
}
void profileTests(Report &report, const fs::path &worker, const fs::path &assets) {
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
        for (auto invalid : {"corrupt", "empty", "signed out", "corrupt cookies", "minimal schema",
                             "missing profile", "missing SDK column"}) {
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
                if (std::string(invalid) == "minimal schema")
                    db.execute("CREATE TABLE account(active INTEGER,dsid INTEGER,id TEXT);"
                               "INSERT INTO account VALUES(1,42,'fixture@example.invalid')");
                else if (std::string(invalid) != "empty") account(db);
                if (std::string(invalid) == "signed out") db.execute("UPDATE account SET active = 0");
                if (std::string(invalid) == "corrupt cookies") write(source / "cookies.sqlitedb", "bad");
                if (std::string(invalid) == "missing profile") db.execute("DROP TABLE profile");
                if (std::string(invalid) == "missing SDK column") db.execute("ALTER TABLE account DROP COLUMN accountFlags");
            }
            require(nmw_import_profile(source.c_str(), dest.c_str()) != NMW_OK, "invalid import succeeded");
            require(read(dest / "keep") == "existing profile" &&
                        !fs::exists(dest / "mpl_db"), "previous profile replaced");
        }
    });
    report.run("current and legacy imports reopen through the real worker", [&] {
        for (auto shape : {"flat", "Android", "marked", "legacy"}) {
            Temp t;
            auto source = t.path / "source", dest = t.path / "dest";
            fs::create_directory(source);
            bool android = std::string(shape) == "Android", legacy = std::string(shape) == "legacy";
            bool flat = std::string(shape) == "flat" || legacy;
            write(source / (android ? "ANDROID_ID" : "native-device-id"), "0123456789abcdef");
            if (std::string(shape) == "marked") write(source / "music-client-v1", "");
            auto folder = flat ? source : source / "mpl_db";
            fs::create_directories(folder);
            Database db(folder / "accounts.sqlitedb");
            db.execute("PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0");
            account(db);
            if (legacy)
                db.execute("ALTER TABLE account DROP COLUMN accountFlags;"
                           "ALTER TABLE profile DROP COLUMN isPrivate;"
                           "ALTER TABLE profile DROP COLUMN isOnBoarded;"
                           "ALTER TABLE profile DROP COLUMN isDiscoverableByContact;"
                           "ALTER TABLE profile DROP COLUMN socialProfileId;"
                           "ALTER TABLE profile DROP COLUMN isOnboardingBlocked;"
                           "ALTER TABLE profile DROP COLUMN isContactsCheckAllowed;"
                           "ALTER TABLE profile DROP COLUMN discoverabilityConsentVersion;"
                           "PRAGMA user_version=1");
            require(nmw_import_profile(source.c_str(), dest.c_str()) == NMW_OK, "usable import rejected");
            wrapper::Worker imported(worker.string(), assets.string(), dest.string());
            NMWOperation operation;
            Bytes request;
            wrapper::appendWord(request, 4);
            auto status = wrapper::json(imported.exchange(request, operation));
            require(status["account_present"] == true && status["account_name"] == "Fixture",
                    "imported account cannot reopen");
            imported.close();
            if (legacy) {
                sqlite3_stmt *statement = nullptr;
                int code = sqlite3_prepare_v2(db.value, "SELECT accountFlags FROM account", -1, &statement, nullptr);
                sqlite3_finalize(statement);
                require(code != SQLITE_OK, "source schema was changed");
            }
            require(read(dest / "native-device-id") == "0123456789abcdef" &&
                        fs::exists(dest / "music-client-v1"), "identity or layout changed");
        }
    });
}
