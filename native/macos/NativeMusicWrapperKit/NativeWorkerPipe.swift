import Foundation
import Darwin

/// All blocking process/pipe work stays on a serial utility queue. The state
/// lock protects ownership only. The inherited pipe and parent watch limit the
/// worker's lifetime to this app; no listener or reusable external proxy exists.
@_spi(Testing) public final class NativeWorkerPipe: NativeWorkerExchanging, @unchecked Sendable {
    private let configuration: NativeRuntimeConfiguration
    private let queue = DispatchQueue(label: "NativeMusicWrapper.worker", qos: .userInitiated)
    private let state = NSLock()
    private var process: NativeChildProcess?
    private var input: FileHandle?
    private var output: FileHandle?
    private var active: UUID?
    private var interrupted = false
    private var version = 0
    public var generation: Int { state.withLock { version } }

    public init(configuration: NativeRuntimeConfiguration) { self.configuration = configuration }
    deinit { interrupt() }

    public func interrupt(_ id: UUID? = nil) {
        state.withLock {
            guard id == nil || active == id else { return }
            interrupted = true
            if let process, process.isRunning { process.terminate() }
        }
    }
    private func reset() {
        let previous = state.withLock { () -> (NativeChildProcess?, FileHandle?, FileHandle?) in
            let value = (process, input, output); process = nil; input = nil; output = nil; interrupted = false; return value
        }
        if let child = previous.0, child.isRunning { child.terminate(); child.waitUntilExit() }
        try? previous.1?.close(); try? previous.2?.close()
    }
    private func start(deadline: TimeInterval) throws -> (Int32, Int32) {
        if let cached = state.withLock({ () -> (Int32, Int32)? in
            guard !interrupted, let process, process.isRunning, let input, let output else { return nil }
            return (input.fileDescriptor, output.fileDescriptor)
        }) { return cached }
        reset()
        try configuration.prepareProfile()
        guard FileManager.default.isExecutableFile(atPath: configuration.binary.path) else { throw NativeWrapperError.unavailable }
        let child = try NativeChildProcess(binary: configuration.binary, arguments: configuration.arguments)
        let writer = child.input, reader = child.output
        for descriptor in [writer.fileDescriptor, reader.fileDescriptor] { _ = fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL) | O_NONBLOCK) }
        _ = fcntl(writer.fileDescriptor, F_SETNOSIGPIPE, 1)
        state.withLock { process = child; input = writer; output = reader; version += 1 }
        guard try response(reader.fileDescriptor, deadline: deadline) == Data("NAD2".utf8) else { throw NativeWrapperError.invalidResponse }
        return (writer.fileDescriptor, reader.fileDescriptor)
    }
    private func ready(_ descriptor: Int32, event: Int16, deadline: TimeInterval) throws {
        let remaining = deadline - ProcessInfo.processInfo.systemUptime
        guard remaining > 0 else { throw NativeWrapperError.timedOut }
        var value = pollfd(fd: descriptor, events: event, revents: 0)
        let result = poll(&value, 1, Int32(min(remaining * 1000, Double(Int32.max))))
        if result < 0 && errno == EINTR { return try ready(descriptor, event: event, deadline: deadline) }
        if result == 0 { throw NativeWrapperError.timedOut }
        guard result > 0, value.revents & event != 0 else { throw NativeWrapperError.unavailable }
    }
    private func write(_ data: Data, descriptor: Int32, deadline: TimeInterval) throws {
        try data.withUnsafeBytes { bytes in
            var offset = 0
            while offset < bytes.count {
                try ready(descriptor, event: Int16(POLLOUT), deadline: deadline)
                let count = Darwin.write(descriptor, bytes.baseAddress!.advanced(by: offset), min(65536, bytes.count - offset))
                if count < 0 && (errno == EINTR || errno == EAGAIN) { continue }
                guard count > 0 else { throw NativeWrapperError.unavailable }; offset += count
            }
        }
    }
    private func read(_ count: Int, descriptor: Int32, deadline: TimeInterval) throws -> Data {
        var data = Data(count: count)
        try data.withUnsafeMutableBytes { bytes in
            var offset = 0
            while offset < count {
                try ready(descriptor, event: Int16(POLLIN), deadline: deadline)
                let size = Darwin.read(descriptor, bytes.baseAddress!.advanced(by: offset), min(65536, count - offset))
                if size < 0 && (errno == EINTR || errno == EAGAIN) { continue }
                guard size > 0 else { throw NativeWrapperError.unavailable }; offset += size
            }
        }
        return data
    }
    private func response(_ descriptor: Int32, deadline: TimeInterval) throws -> Data {
        let prefix = try read(4, descriptor: descriptor, deadline: deadline)
        let size = try NativeFrame.readWord(prefix, at: 0)
        guard size > 0, size <= 16 * 1024 * 1024 else { throw NativeWrapperError.oversized }
        let data = try read(size, descriptor: descriptor, deadline: deadline)
        guard data.first == 0 else { throw NativeWrapperError.rejected }
        return Data(data.dropFirst())
    }
    public func exchange(_ request: Data) async throws -> Data {
        try Task.checkCancellation()
        guard request.count <= 16 * 1024 * 1024 else { throw NativeWrapperError.oversized }
        let id = UUID(), pending = NativeRequestCompletion<Data>()
        return try await withTaskCancellationHandler {
            let result: Data = try await withCheckedThrowingContinuation { continuation in
                guard pending.install(continuation) else { return }
                queue.async { [self] in
                    guard !pending.isComplete else { return }
                    state.withLock { active = id }
                    defer { state.withLock { active = nil } }
                    do {
                        let deadline = ProcessInfo.processInfo.systemUptime + 65
                        let (writer, reader) = try start(deadline: deadline)
                        guard !pending.isComplete else { reset(); return }
                        try write(NativeFrame.word(request.count) + request, descriptor: writer, deadline: deadline)
                        pending.finish(.success(try response(reader, deadline: deadline)))
                    } catch {
                        reset(); pending.finish(.failure(error))
                    }
                }
            }
            try Task.checkCancellation(); return result
        } onCancel: { [self] in interrupt(id); pending.finish(.failure(CancellationError())) }
    }
    public func close() async {
        interrupt()
        await withCheckedContinuation { continuation in
            queue.async { [self] in reset(); continuation.resume() }
        }
    }
}
