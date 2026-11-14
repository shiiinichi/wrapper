import Foundation

@_spi(Testing) public protocol NativeHTTPFetching: Sendable {
    func fetch(_ request: URLRequest) async throws -> Data
}

/// URLSession delivers decompressed chunks. Cancel before accumulating beyond
/// the limit, and reject cross-origin redirects before forwarding credentials.
@_spi(Testing) public final class NativeHTTPSClient: NSObject, NativeHTTPFetching, URLSessionDataDelegate, @unchecked Sendable {
    private final class Delegate: NSObject, URLSessionDataDelegate, @unchecked Sendable {
        weak var owner: NativeHTTPSClient?
        func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                        newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
            guard let owner else { completionHandler(nil); return }
            owner.urlSession(session, task: task, willPerformHTTPRedirection: response, newRequest: request, completionHandler: completionHandler)
        }
        func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                        completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void) {
            guard let owner else { completionHandler(.cancel); return }
            owner.urlSession(session, dataTask: dataTask, didReceive: response, completionHandler: completionHandler)
        }
        func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) { owner?.urlSession(session, dataTask: dataTask, didReceive: data) }
        func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) { owner?.urlSession(session, task: task, didCompleteWithError: error) }
        func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, willCacheResponse proposedResponse: CachedURLResponse,
                        completionHandler: @escaping @Sendable (CachedURLResponse?) -> Void) { completionHandler(nil) }
    }
    private final class Transfer: @unchecked Sendable {
        let completion = NativeRequestCompletion<Data>()
        var bytes = Data() // Protected by the client's lock.
        private let lock = NSLock()
        private var task: URLSessionDataTask?
        func attach(_ task: URLSessionDataTask) { lock.withLock { self.task = task }; if completion.isComplete { task.cancel() } }
        func cancel() { completion.finish(.failure(CancellationError())); lock.withLock { task?.cancel() } }
    }
    private let lock = NSLock()
    private var transfers: [Int: Transfer] = [:]
    private var session: URLSession!
    public static let maximumBytes = 32 * 1024 * 1024

    public init(configuration: URLSessionConfiguration = .ephemeral) {
        super.init()
        configuration.urlCache = nil; configuration.httpCookieStorage = nil
        configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
        configuration.timeoutIntervalForRequest = 25
        configuration.timeoutIntervalForResource = 30
        configuration.connectionProxyDictionary = [:]
        let delegate = Delegate(); delegate.owner = self
        session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
    }
    public func close() { session.invalidateAndCancel() }
    deinit { session.invalidateAndCancel() }

    public static func permitsRedirect(from: URL, to: URL) -> Bool {
        from.scheme == "https" && to.scheme == "https" && from.host == to.host
            && (from.port ?? 443) == (to.port ?? 443) && to.user == nil && to.password == nil
    }
    public func fetch(_ request: URLRequest) async throws -> Data {
        guard let url = request.url, url.scheme == "https", url.host != nil, url.user == nil, url.password == nil else {
            throw NativeWrapperError.message("Apple requests require HTTPS.")
        }
        try Task.checkCancellation()
        let transfer = Transfer()
        return try await withTaskCancellationHandler {
            let data: Data = try await withCheckedThrowingContinuation { continuation in
                guard transfer.completion.install(continuation) else { return }
                let task = session.dataTask(with: request)
                lock.withLock { transfers[task.taskIdentifier] = transfer }
                transfer.attach(task)
                task.resume()
            }
            try Task.checkCancellation(); return data
        } onCancel: { transfer.cancel() }
    }
    public func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        guard let old = response.url, let next = request.url, Self.permitsRedirect(from: old, to: next) else {
            finish(task.taskIdentifier, .failure(NativeWrapperError.message("Apple redirected outside the trusted HTTPS origin.")))
            completionHandler(nil); return
        }
        completionHandler(request)
    }
    public func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping @Sendable (URLSession.ResponseDisposition) -> Void) {
        guard let response = response as? HTTPURLResponse, (200...299).contains(response.statusCode) else {
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            finish(dataTask.taskIdentifier, .failure(NativeWrapperError.httpStatus(code)))
            completionHandler(.cancel); return
        }
        guard response.expectedContentLength <= Self.maximumBytes else {
            finish(dataTask.taskIdentifier, .failure(NativeWrapperError.oversized)); completionHandler(.cancel); return
        }
        completionHandler(.allow)
    }
    public func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        let excessive = lock.withLock { () -> Bool in
            guard let transfer = transfers[dataTask.taskIdentifier] else { return false }
            guard data.count <= Self.maximumBytes - transfer.bytes.count else { return true }
            transfer.bytes.append(data); return false
        }
        if excessive { finish(dataTask.taskIdentifier, .failure(NativeWrapperError.oversized)); dataTask.cancel() }
    }
    public func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error {
            if (error as? URLError)?.code == .cancelled { finish(task.taskIdentifier, .failure(CancellationError())) }
            else { finish(task.taskIdentifier, .failure(NativeWrapperError.message("Apple network request failed."))) }
        } else {
            let data = lock.withLock { transfers[task.taskIdentifier]?.bytes ?? Data() }
            finish(task.taskIdentifier, .success(data))
        }
    }
    public func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, willCacheResponse proposedResponse: CachedURLResponse,
                    completionHandler: @escaping @Sendable (CachedURLResponse?) -> Void) { completionHandler(nil) }
    private func finish(_ id: Int, _ result: Result<Data, Error>) {
        lock.withLock { transfers.removeValue(forKey: id) }?.completion.finish(result)
    }
}
