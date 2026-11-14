import Foundation
import Darwin

/// POSIX ownership avoids Process.waitUntilExit's run-loop dependency on a
/// dispatch worker thread. Only this child's PID is signalled and reaped.
@_spi(Testing) public final class NativeChildProcess: @unchecked Sendable {
    public let processIdentifier: pid_t
    public let input: FileHandle, output: FileHandle
    private let lock = NSLock()
    private var reaped = false

    public init(binary: URL, arguments: [String]) throws {
        var incoming: [Int32] = [0, 0], outgoing: [Int32] = [0, 0]
        guard pipe(&incoming) == 0 else { throw NativeWrapperError.unavailable }
        guard pipe(&outgoing) == 0 else { close(incoming[0]); close(incoming[1]); throw NativeWrapperError.unavailable }
        var launched = false
        defer {
            close(incoming[0]); close(outgoing[1])
            if !launched { close(incoming[1]); close(outgoing[0]) }
        }
        for fd in incoming + outgoing { _ = fcntl(fd, F_SETFD, FD_CLOEXEC) }
        var actions: posix_spawn_file_actions_t?
        guard posix_spawn_file_actions_init(&actions) == 0 else { throw NativeWrapperError.unavailable }
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_adddup2(&actions, incoming[0], STDIN_FILENO)
        posix_spawn_file_actions_adddup2(&actions, outgoing[1], STDOUT_FILENO)
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0)
        for fd in incoming + outgoing { posix_spawn_file_actions_addclose(&actions, fd) }
        var attributes: posix_spawnattr_t?
        guard posix_spawnattr_init(&attributes) == 0 else { throw NativeWrapperError.unavailable }
        defer { posix_spawnattr_destroy(&attributes) }
        posix_spawnattr_setflags(&attributes, Int16(POSIX_SPAWN_CLOEXEC_DEFAULT))
        let environment = ["HOME=" + FileManager.default.homeDirectoryForCurrentUser.path,
                           "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "TMPDIR=" + NSTemporaryDirectory(), "LANG=en_US.UTF-8"]
        let argv = ([binary.path] + arguments).map { strdup($0) } + [nil]
        let env = environment.map { strdup($0) } + [nil]
        defer { argv.forEach { free($0) }; env.forEach { free($0) } }
        var pid: pid_t = 0
        let result = argv.withUnsafeBufferPointer { args in
            env.withUnsafeBufferPointer { values in
                posix_spawn(&pid, binary.path, &actions, &attributes, UnsafeMutablePointer(mutating: args.baseAddress!), UnsafeMutablePointer(mutating: values.baseAddress!))
            }
        }
        guard result == 0 else { throw NativeWrapperError.unavailable }
        processIdentifier = pid; launched = true
        input = FileHandle(fileDescriptor: incoming[1], closeOnDealloc: true)
        output = FileHandle(fileDescriptor: outgoing[0], closeOnDealloc: true)
    }
    public var isRunning: Bool {
        lock.withLock {
            guard !reaped else { return false }
            var status: Int32 = 0
            let result = waitpid(processIdentifier, &status, WNOHANG)
            if result == processIdentifier || (result < 0 && errno == ECHILD) { reaped = true; return false }
            return true
        }
    }
    public func terminate() {
        lock.withLock {
            guard !reaped else { return }
            var status: Int32 = 0
            let result = waitpid(processIdentifier, &status, WNOHANG)
            if result == 0 { kill(processIdentifier, SIGKILL) }
            else if result == processIdentifier || (result < 0 && errno == ECHILD) { reaped = true }
        }
    }
    public func waitUntilExit() {
        let deadline = ProcessInfo.processInfo.systemUptime + 2
        while isRunning, ProcessInfo.processInfo.systemUptime < deadline { usleep(1000) }
    }
    deinit { terminate(); waitUntilExit() }
}
