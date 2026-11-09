import Foundation

struct WrapperError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

@MainActor
final class EndpointClient {
    let baseURL: URL
    private let session: URLSession
    private(set) var lastResponseMilliseconds: Double?

    func resetResponseTiming() { lastResponseMilliseconds = nil }

    init(port: Int) {
        baseURL = URL(string: "http://127.0.0.1:\(port)")!
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 90
        configuration.timeoutIntervalForResource = 100
        configuration.connectionProxyDictionary = [:]
        session = URLSession(configuration: configuration)
    }

    func get(_ route: String, _ parameters: [String: String] = [:]) async throws -> [String: Any] {
        let started = ProcessInfo.processInfo.systemUptime
        defer { lastResponseMilliseconds = (ProcessInfo.processInfo.systemUptime - started) * 1000 }
        var components = URLComponents(url: baseURL.appendingPathComponent(route), resolvingAgainstBaseURL: false)!
        components.queryItems = parameters.map { URLQueryItem(name: $0.key, value: $0.value) }
        let (data, response) = try await session.data(from: components.url!)
        guard let http = response as? HTTPURLResponse,
              http.value(forHTTPHeaderField: "X-Wrapper-Backend") == "native-macos-arm64" else {
            throw WrapperError(message: "This port is not serving the native wrapper.")
        }
        guard let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let code = envelope["code"] as? Int else {
            throw WrapperError(message: "The server returned an invalid response.")
        }
        guard code == 0 else { throw WrapperError(message: envelope["msg"] as? String ?? "The request failed.") }
        guard let result = envelope["data"] as? [String: Any] else {
            throw WrapperError(message: "The server returned no data.")
        }
        return result
    }

    func manifest(_ url: String) async throws -> String? {
        var address = URL(string: url)
        for _ in 0..<4 {
            guard let current = address, current.scheme == "https", let host = current.host,
                  ["apple.com", "mzstatic.com"].contains(where: { host == $0 || host.hasSuffix("." + $0) }) else {
                throw WrapperError(message: "The manifest URL is not an Apple HTTPS address.")
            }
            let (data, response) = try await session.data(from: current)
            guard (response as? HTTPURLResponse)?.statusCode == 200, data.count <= 2 * 1024 * 1024,
                  let text = String(data: data, encoding: .utf8), text.hasPrefix("#EXTM3U") else {
                throw WrapperError(message: "The returned playlist could not be validated.")
            }
            let keys = text.matches(#"#EXT-X-(?:SESSION-)?KEY:[^\r\n]*URI="(skd://[^"]+)""#)
            if let key = keys.first(where: { $0 != "skd://itunes.apple.com/P000000000/s1/e1" }) { return key }
            if !text.contains("#EXT-X-STREAM-INF:") && !text.contains("#EXT-X-MEDIA:") { return nil }
            let children = text.matches(#"#EXT-X-MEDIA:[^\r\n]*URI="([^"]+)""#)
                + text.components(separatedBy: .newlines).filter { !$0.isEmpty && !$0.hasPrefix("#") }
            guard let child = children.first else { return nil }
            address = URL(string: child, relativeTo: current)?.absoluteURL
        }
        return nil
    }

    func invalidate() { session.invalidateAndCancel() }
}

private extension String {
    func matches(_ expression: String) -> [String] {
        guard let regex = try? NSRegularExpression(pattern: expression) else { return [] }
        return regex.matches(in: self, range: NSRange(startIndex..., in: self)).compactMap {
            guard let range = Range($0.range(at: 1), in: self) else { return nil }
            return String(self[range])
        }
    }
}
