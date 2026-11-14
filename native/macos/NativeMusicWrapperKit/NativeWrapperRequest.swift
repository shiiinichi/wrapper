import Foundation

public struct WrapperRequest: Sendable {
    public let route: String
    public var query: [String: String]
    public var body: Data
    public var context: String

    public init(route: String, query: [String: String] = [:], body: Data = Data(), context: String = "") {
        self.route = route
        self.query = query
        self.body = body
        self.context = context
    }
}
public struct WrapperReply: Sendable {
    public let metadata: Data
    public var binary: Data

    public init(metadata: Data, binary: Data = Data()) {
        self.metadata = metadata
        self.binary = binary
    }
}
public protocol WrapperRequestSending: Sendable {
    var cacheIdentity: String { get }
    func send(_ request: WrapperRequest) async throws -> WrapperReply
}
public enum NativeWrapperLimits {
    public static let maximumMetadata = 64 * 1024
    public static let maximumBody = 16 * 1024 * 1024 - 8
    public static let maximumReply = 32 * 1024 * 1024
}
public enum NativeWrapperError: LocalizedError {
    case unavailable, timedOut, oversized, invalidResponse, busy, rejected, httpStatus(Int), message(String)
    public var errorDescription: String? {
        switch self {
        case .unavailable: "The embedded native music engine is unavailable."
        case .timedOut: "The native music engine timed out."
        case .oversized: "The native request or reply exceeds its size limit."
        case .invalidResponse: "The native music engine returned an invalid response."
        case .busy: "The native music engine is busy. Try again shortly."
        case .rejected: "The native operation failed. Check the native account in Settings."
        case .httpStatus(let status): "Apple returned HTTP \(status)."
        case .message(let value): value
        }
    }
}

@_spi(Testing) public enum NativeFrame {
    public static func word(_ value: Int) -> Data { var value = UInt32(value).bigEndian; return withUnsafeBytes(of: &value) { Data($0) } }
    public static func string(_ value: String) -> Data { let bytes = Data(value.utf8); return word(bytes.count) + bytes }
    public static func readWord(_ data: Data, at offset: Int) throws -> Int {
        guard offset >= 0, offset <= data.count - 4 else { throw NativeWrapperError.invalidResponse }
        return data[offset..<(offset + 4)].reduce(0) { ($0 << 8) | Int($1) }
    }
}

/// Cancellation may arrive before a continuation is installed. Completed values
/// are released immediately, including multi-megabyte sample batches.
@_spi(Testing) public final class NativeRequestCompletion<Value: Sendable>: @unchecked Sendable {
    public init() {}
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Value, Error>?
    private var early: Result<Value, Error>?
    private var completed = false
    public var isComplete: Bool { lock.withLock { completed } }
    public func install(_ value: CheckedContinuation<Value, Error>) -> Bool {
        let result: Result<Value, Error>? = lock.withLock {
            if let early { self.early = nil; return early }
            if completed { return .failure(CancellationError()) }
            continuation = value; return nil
        }
        if let result { value.resume(with: result); return false }; return true
    }
    public func finish(_ result: Result<Value, Error>) {
        let target: CheckedContinuation<Value, Error>? = lock.withLock {
            guard !completed else { return nil }; completed = true
            let value = continuation; continuation = nil
            if value == nil { early = result }; return value
        }
        target?.resume(with: result)
    }
}
