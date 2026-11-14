import Foundation
import Darwin

/// Import a stopped SDK profile into app-owned storage. Never follow links or
/// mutate the source. Stage the complete bounded copy before replacing anything.
@_spi(Testing) public enum NativeProfileImporter {
    public static func replace(from source: URL, destination: URL) throws {
        let manager = FileManager.default
        let source = source.standardizedFileURL, destination = destination.standardizedFileURL
        guard source != destination, !destination.path.hasPrefix(source.path + "/"),
              !source.path.hasPrefix(destination.path + "/") else { throw NativeWrapperError.message("Choose the existing wrapper’s account folder.") }
        let root = try source.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
        guard root.isDirectory == true, root.isSymbolicLink != true else { throw NativeWrapperError.message("The account folder must be a real directory.") }
        let sourceFD = open(source.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard sourceFD >= 0 else { throw NativeWrapperError.message("The source account folder could not be opened.") }
        defer { close(sourceFD) }
        let descriptor = openat(sourceFD, "native-api.lock", O_RDONLY | O_NOFOLLOW | O_CLOEXEC)
        if descriptor >= 0 {
            guard flock(descriptor, LOCK_EX | LOCK_NB) == 0 else { close(descriptor); throw NativeWrapperError.message("Quit Native Music Wrapper before importing its account.") }
        } else if errno != ENOENT { throw NativeWrapperError.message("The source account folder could not be locked.") }
        defer { if descriptor >= 0 { flock(descriptor, LOCK_UN); close(descriptor) } }
        let parent = destination.deletingLastPathComponent()
        try manager.createDirectory(at: parent, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let stage = parent.appendingPathComponent(".native-import-" + UUID().uuidString)
        let backup = parent.appendingPathComponent(".native-backup-" + UUID().uuidString)
        try manager.createDirectory(at: stage, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? manager.removeItem(at: stage) }
        var total: Int64 = 0
        let databaseNames = ["accounts", "cookies", "httpcache", "kvs"].flatMap { name in
            ["\(name).sqlitedb", "\(name).sqlitedb-wal", "\(name).sqlitedb-shm"]
        }
        // Inspect only these 14 known files. Never enumerate or descend into
        // unrelated logs, policy records, or potentially large backup folders.
        for name in databaseNames + ["native-device-id", "adi.pb"] {
            let input = openat(sourceFD, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)
            if input < 0 && errno == ENOENT { continue }
            guard input >= 0 else { throw NativeWrapperError.message("A native account file could not be opened. Symbolic links are not supported.") }
            defer { close(input) }
            var info = stat()
            guard fstat(input, &info) == 0, info.st_mode & S_IFMT == S_IFREG, info.st_uid == getuid(), info.st_size >= 0,
                  info.st_size <= 64 * 1024 * 1024 else { throw NativeWrapperError.message("Invalid native account file.") }
            total += info.st_size; guard total <= 128 * 1024 * 1024 else { throw NativeWrapperError.oversized }
            let outputFD = open(stage.appendingPathComponent(name).path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0o600)
            guard outputFD >= 0 else { throw NativeWrapperError.message("The native account could not be copied.") }
            defer { close(outputFD) }
            var bytes = [UInt8](repeating: 0, count: 65536), remaining = info.st_size
            while remaining > 0 {
                let readCount = read(input, &bytes, min(bytes.count, Int(remaining)))
                if readCount < 0 && errno == EINTR { continue }
                guard readCount > 0 else { throw NativeWrapperError.invalidResponse }
                try bytes.withUnsafeBytes { buffer in
                    var sent = 0
                    while sent < readCount {
                        let written = write(outputFD, buffer.baseAddress!.advanced(by: sent), readCount - sent)
                        if written < 0 && errno == EINTR { continue }
                        guard written > 0 else { throw NativeWrapperError.unavailable }; sent += written
                    }
                }
                remaining -= Int64(readCount)
            }
        }
        let identity = try Data(contentsOf: stage.appendingPathComponent("native-device-id"))
        guard identity.count == 16, identity.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
              manager.fileExists(atPath: stage.appendingPathComponent("accounts.sqlitedb").path) else {
            throw NativeWrapperError.message("This folder does not contain a native wrapper account.")
        }
        let hadPrevious = manager.fileExists(atPath: destination.path)
        if hadPrevious { try manager.moveItem(at: destination, to: backup) }
        do { try manager.moveItem(at: stage, to: destination) }
        catch { if hadPrevious { try? manager.moveItem(at: backup, to: destination) }; throw error }
        if hadPrevious { try? manager.removeItem(at: backup) }
    }
}
