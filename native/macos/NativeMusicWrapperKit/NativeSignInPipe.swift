import Foundation
import Darwin

public protocol NativeAccountSigningIn: Sendable {
    func run(prompt: @escaping @Sendable (NativeAuthPrompt) -> Void) async throws
    func submit(_ answer: String)
    func cancel()
}

public struct NativeAuthPrompt: Sendable, Identifiable {
    public let id = UUID()
    public let kind: String
    public var title = "Apple Account Sign In", message = "", buttons: [String] = []
    public static func decode(_ data: Data) throws -> Self {
        guard let json = try JSONSerialization.jsonObject(with: data) as? [String: String],
              let kind = json["prompt"], ["credentials", "code", "dialog"].contains(kind) else { throw NativeWrapperError.invalidResponse }
        var value = Self(kind: kind)
        if kind == "dialog" {
            guard let text = json["plist"], let fields = try PropertyListSerialization.propertyList(from: Data(text.utf8), format: nil) as? [String: Any],
                  let title = fields["title"] as? String, let message = fields["message"] as? String,
                  let buttons = fields["buttons"] as? [String], (1...32).contains(buttons.count) else { throw NativeWrapperError.invalidResponse }
            value.title = title; value.message = message; value.buttons = buttons
        }
        return value
    }
}

/// Interactive authentication uses the same signed app-owned helper, with
/// credentials exchanged through inherited pipes. No credential is in argv.
public final class NativeSignInPipe: NativeAccountSigningIn, @unchecked Sendable {
    private let configuration: NativeRuntimeConfiguration
    private let operation: String
    private let lock = NSLock()
    private let queue = DispatchQueue(label: "NativeMusicWrapper.sign-in", qos: .userInitiated)
    private let writes = DispatchQueue(label: "NativeMusicWrapper.sign-in-input")
    private var process: NativeChildProcess?
    private var input: FileHandle?
    private var cancelled = false, awaitingInput = false
    public init(configuration: NativeRuntimeConfiguration, operation: String = "auth-login") {
        self.configuration = configuration; self.operation = operation
    }
    deinit { cancel() }
    public func cancel() {
        lock.withLock {
            cancelled = true; awaitingInput = false
            if let process, process.isRunning { process.terminate() }
        }
    }
    public func submit(_ answer: String) {
        guard !answer.isEmpty, answer.utf8.count <= 8192 else { return }
        writes.async { [self] in
            let descriptor = lock.withLock { () -> Int32? in
                guard awaitingInput, !cancelled, let input else { return nil }
                awaitingInput = false
                // Retain this pipe while the read queue cleans up after a
                // concurrent cancellation; a reused FD must never get secrets.
                let duplicate = fcntl(input.fileDescriptor, F_DUPFD_CLOEXEC, 3)
                return duplicate >= 0 ? duplicate : nil
            }
            guard let descriptor else { return }
            defer { close(descriptor) }
            _ = fcntl(descriptor, F_SETNOSIGPIPE, 1)
            var data = NativeFrame.string(answer)
            defer { data.resetBytes(in: data.startIndex..<data.endIndex) }
            do {
                try data.withUnsafeBytes { buffer in
                    var offset = 0
                    let deadline = ProcessInfo.processInfo.systemUptime + 3
                    while offset < buffer.count {
                        try Self.ready(descriptor, event: Int16(POLLOUT), deadline: deadline)
                        let count = write(descriptor, buffer.baseAddress!.advanced(by: offset), buffer.count - offset)
                        if count < 0 && (errno == EINTR || errno == EAGAIN) { continue }
                        guard count > 0 else { throw NativeWrapperError.unavailable }; offset += count
                    }
                }
            } catch { cancel() }
        }
    }
    private static func ready(_ fd: Int32, event: Int16, deadline: Double) throws {
        let remaining = deadline - ProcessInfo.processInfo.systemUptime
        guard remaining > 0 else { throw NativeWrapperError.timedOut }
        var descriptor = pollfd(fd: fd, events: event, revents: 0)
        let result = poll(&descriptor, 1, Int32(min(remaining * 1000, Double(Int32.max))))
        if result < 0 && errno == EINTR { return try ready(fd, event: event, deadline: deadline) }
        if result == 0 { throw NativeWrapperError.timedOut }
        guard result > 0, descriptor.revents & (event | Int16(POLLHUP)) != 0 else { throw NativeWrapperError.unavailable }
    }
    private static func readBytes(_ fd: Int32, count: Int, deadline: Double, allowEOF: Bool = false) throws -> Data {
        var bytes = Data(count: count), offset = 0
        try bytes.withUnsafeMutableBytes { buffer in
            while offset < count {
                try ready(fd, event: Int16(POLLIN), deadline: deadline)
                let size = read(fd, buffer.baseAddress!.advanced(by: offset), count - offset)
                if size < 0 && (errno == EINTR || errno == EAGAIN) { continue }
                if size == 0 && allowEOF { break }
                guard size > 0 else { throw NativeWrapperError.unavailable }; offset += size
            }
        }
        bytes.count = offset; return bytes
    }
    public func run(prompt: @escaping @Sendable (NativeAuthPrompt) -> Void) async throws {
        let pending = NativeRequestCompletion<Void>()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                guard pending.install(continuation) else { return }
                queue.async { [self] in
                    do {
                        try configuration.prepareProfile()
                        let arguments = ["--assets", configuration.assets.path, "--apple-profile", "--probe", operation, "--data-dir", configuration.profile.path,
                                         "--prompt-script", "@private-pipe", "--owner-pid", String(ProcessInfo.processInfo.processIdentifier)]
                        let child = try lock.withLock { () -> NativeChildProcess in
                            guard !cancelled else { throw CancellationError() }
                            let child = try NativeChildProcess(binary: configuration.binary, arguments: arguments)
                            process = child; input = child.input; return child
                        }
                        defer {
                            if child.isRunning { child.terminate() }; child.waitUntilExit()
                            try? child.input.close(); try? child.output.close()
                            lock.withLock { process = nil; input = nil; awaitingInput = false }
                        }
                        let reader = child.output.fileDescriptor
                        _ = fcntl(reader, F_SETFL, fcntl(reader, F_GETFL) | O_NONBLOCK)
                        _ = fcntl(child.input.fileDescriptor, F_SETNOSIGPIPE, 1)
                        _ = fcntl(child.input.fileDescriptor, F_SETFL, fcntl(child.input.fileDescriptor, F_GETFL) | O_NONBLOCK)
                        let deadline = ProcessInfo.processInfo.systemUptime + 600
                        while true {
                            let header = try Self.readBytes(reader, count: 4, deadline: deadline)
                            if header.first == 123 { // Final native JSON report; prompts are length-prefixed.
                                var report = header
                                while true {
                                    let chunk = try Self.readBytes(reader, count: 4096, deadline: deadline, allowEOF: true)
                                    guard report.count + chunk.count <= 65536 else { throw NativeWrapperError.oversized }
                                    report.append(chunk); if chunk.count < 4096 { break }
                                }
                                guard let root = try JSONSerialization.jsonObject(with: report) as? [String: Any], root["ok"] as? Bool == true,
                                      let result = root["result"] as? [String: Any], result["authenticated"] as? Bool == true,
                                      result["account_present"] as? Bool == true else { throw NativeWrapperError.message("Apple Account sign-in did not complete. Please try again.") }
                                break
                            }
                            let count = try NativeFrame.readWord(header, at: 0)
                            guard count > 0, count <= 65536 else { throw NativeWrapperError.invalidResponse }
                            let value = try NativeAuthPrompt.decode(Self.readBytes(reader, count: count, deadline: deadline))
                            lock.withLock { awaitingInput = true }; prompt(value)
                        }
                        pending.finish(.success(()))
                    } catch { pending.finish(.failure(lock.withLock { cancelled } ? CancellationError() : error)) }
                }
            }
        } onCancel: { [self] in cancel() }
    }
}
