import Foundation

@_spi(Testing) public protocol NativeWorkerExchanging: Sendable {
    var generation: Int { get }
    func exchange(_ request: Data) async throws -> Data
    func close() async
}

/// The app owns this engine and its inherited pipes. There is no external
/// wrapper endpoint, shared authorization token, or separately launched service.
public actor EmbeddedWrapperBackend: WrapperRequestSending {
    private let configuration: NativeRuntimeConfiguration
    private let worker: any NativeWorkerExchanging
    private let web: NativeAppleWebAPI
    private let gate = NativeRequestGate()
    private let artistGate = NativeRequestGate()
    private nonisolated let identity = NativeAccountIdentity()
    public nonisolated var cacheIdentity: String { "embedded:" + identity.value }
    private var tokens: [String: String] = [:]
    private var tokenTime = 0.0
    private var opens = 0
    private struct Context {
        let adam: String, uri: String
        var used: Double, generation = -1, handle = 0, content = ""
    }
    private var contexts: [String: Context] = [:]
    private var accountOperation = false

    public init(configuration: NativeRuntimeConfiguration) {
        self.init(configuration: configuration, worker: NativeWorkerPipe(configuration: configuration), http: NativeHTTPSClient())
    }

    @_spi(Testing) public init(configuration: NativeRuntimeConfiguration, worker: any NativeWorkerExchanging, http: any NativeHTTPFetching) {
        self.configuration = configuration
        self.worker = worker
        web = NativeAppleWebAPI(http: http)
    }
    public func send(_ request: WrapperRequest) async throws -> WrapperReply {
        let isArtist = request.route == "/artist"
        guard isArtist || !accountOperation else { throw NativeWrapperError.busy }
        // A slow public catalog response must not hold the pipe used for audio samples.
        let admission = isArtist ? artistGate : gate
        try await admission.acquire()
        do {
            try Task.checkCancellation()
            guard isArtist || !accountOperation else { throw NativeWrapperError.busy }
            let reply = try await perform(request)
            await admission.release(); return reply
        } catch { await admission.release(); throw error }
    }
    private func envelope(_ data: Data, code: Int = 0, message: String = "success", binary: Data = Data()) throws -> WrapperReply {
        guard data.count <= NativeWrapperLimits.maximumReply else { throw NativeWrapperError.oversized }
        let payload = try JSONSerialization.jsonObject(with: data, options: .fragmentsAllowed)
        let metadata = try JSONSerialization.data(withJSONObject: ["code": code, "msg": message, "data": payload], options: [.sortedKeys, .fragmentsAllowed])
        guard metadata.count <= NativeWrapperLimits.maximumReply else { throw NativeWrapperError.oversized }
        return WrapperReply(metadata: metadata, binary: binary)
    }
    private func accountTokens() async throws -> [String: String] {
        let now = ProcessInfo.processInfo.systemUptime
        if !tokens.isEmpty, now - tokenTime < 1800 { return tokens }
        let data = try await worker.exchange(NativeFrame.word(5))
        guard data.count <= 512 * 1024, let value = try JSONSerialization.jsonObject(with: data) as? [String: String] else {
            throw NativeWrapperError.invalidResponse
        }
        tokens = value; tokenTime = now; return value
    }
    private func identifier(_ request: WrapperRequest, prefetch: Bool = false) throws -> String {
        try identifier(request.query["adamId"] ?? "", prefetch: prefetch)
    }
    private func identifier(_ value: String, prefetch: Bool = false) throws -> String {
        guard !value.isEmpty, value.utf8.count <= 19, value.utf8.allSatisfy({ (48...57).contains($0) }),
              let number = Int64(value), number > 0 || (prefetch && value == "0") else {
            throw NativeWrapperError.message("Invalid catalog track ID.")
        }
        return value
    }
    private func perform(_ request: WrapperRequest) async throws -> WrapperReply {
        guard request.body.count <= NativeWrapperLimits.maximumBody,
              request.query.count <= 8, request.query.allSatisfy({ $0.key.utf8.count <= 64 && $0.value.utf8.count <= 8192 }),
              request.context.utf8.count <= 48 else { throw NativeWrapperError.oversized }
        switch request.route {
        case "/artist":
            let rawID = request.query["adamId"] ?? ""
            guard rawID.utf8.count <= 19, rawID.utf8.allSatisfy({ (48...57).contains($0) }),
                  let id = Int64(rawID), id > 0 else {
                return try envelope(Data("null".utf8), code: 400, message: "invalid adamId")
            }
            let storefront = request.query["storefront"] ?? "us"
            guard storefront.utf8.count == 2, storefront.utf8.allSatisfy({ (65...90).contains($0) || (97...122).contains($0) }) else {
                return try envelope(Data("null".utf8), code: 400, message: "invalid storefront")
            }
            guard let profile = try await web.artist(adam: String(id), storefront: storefront.lowercased()) else {
                return try envelope(Data("null".utf8), code: 404, message: "artist not found")
            }
            return try envelope(profile)
        case "/status":
            let data = try await worker.exchange(NativeFrame.word(4))
            guard var status = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { throw NativeWrapperError.invalidResponse }
            status["transport"] = "embedded"; status["temari_compatible"] = false
            return try envelope(JSONSerialization.data(withJSONObject: status))
        case "/m3u8":
            let adam = try identifier(request)
            return try await envelope(web.manifest(adam: adam, tokens: accountTokens()))
        case "/webplayback":
            let adam = try identifier(request)
            return try await envelope(web.webPlayback(adam: adam, tokens: accountTokens()))
        case "/assets":
            let adam = try identifier(request)
            return try await envelope(web.assets(adam: adam, tokens: accountTokens()))
        case "/license":
            guard request.body.count <= NativeWrapperLimits.maximumMetadata else { throw NativeWrapperError.oversized }
            guard let body = try JSONSerialization.jsonObject(with: request.body) as? [String: Any],
                  let raw = body["adamId"] as? String, let challenge = body["challenge"] as? String,
                  let uri = body["uri"] as? String,
                  let drm = NativeWebLicenseChallenge.DRM(rawValue: body["drm-type"] as? String ?? "wv") else {
                throw NativeWrapperError.message("A catalog ID, license challenge, key URI, and valid DRM type are required.")
            }
            let adam = try identifier(raw)
            let license = NativeWebLicenseChallenge(challenge: challenge, uri: uri, drm: drm)
            try license.validate()
            return try await envelope(web.license(adam: adam, challenge: license, tokens: accountTokens()))
        case "/lyrics":
            let adam = try identifier(request)
            return try await envelope(web.lyrics(adam: adam, tokens: accountTokens(), language: request.query["language"] ?? "en",
                                                script: request.query["script"] ?? "en-Latn", syllable: !["0", "false", "no", "off"].contains(request.query["syllable"]?.lowercased() ?? "1")))
        case "/key":
            let adam = try identifier(request, prefetch: true), uri = request.query["uri"] ?? ""
            guard uri.hasPrefix("skd://"), uri.utf8.count <= 8192, !uri.utf8.contains(where: { $0 < 32 || $0 == 127 }),
                  adam == "0" || uri != "skd://itunes.apple.com/P000000000/s1/e1" else { throw NativeWrapperError.message("Invalid native key URI.") }
            prune()
            let token = contexts.first { $0.value.adam == adam && $0.value.uri == uri }?.key
                ?? (0..<24).map { _ in String(format: "%02x", UInt8.random(in: .min ... .max)) }.joined()
            var entry = contexts[token] ?? Context(adam: adam, uri: uri, used: ProcessInfo.processInfo.systemUptime)
            try await ensure(&entry)
            entry.used = ProcessInfo.processInfo.systemUptime; contexts[token] = entry; prune()
            return try envelope(JSONSerialization.data(withJSONObject: ["adamId": adam, "keyUri": uri, "contentKey": entry.content,
                         "temariCompatible": false, "nativeDecryptionVersion": 1, "nativeContext": token]))
        case "/decrypt":
            let sizes = try Self.sampleSizes(request.body)
            prune()
            guard request.context.utf8.count == 48, request.context.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
                  var entry = contexts[request.context] else {
                return try envelope(Data("null".utf8), code: 409, message: "Native context expired; request the key again.")
            }
            try await ensure(&entry)
            let response = try await worker.exchange(NativeFrame.word(2) + NativeFrame.word(entry.handle) + request.body)
            guard (try? Self.sampleSizes(response)) == sizes else { await worker.close(); throw NativeWrapperError.invalidResponse }
            entry.used = ProcessInfo.processInfo.systemUptime; contexts[request.context] = entry
            return try envelope(Data("null".utf8), binary: response)
        default: throw NativeWrapperError.message("Unknown native music operation.")
        }
    }
    private func prune() {
        let cutoff = ProcessInfo.processInfo.systemUptime - 1800
        contexts = contexts.filter { $0.value.used >= cutoff }
        while contexts.count > 32, let oldest = contexts.min(by: { $0.value.used < $1.value.used })?.key { contexts.removeValue(forKey: oldest) }
    }
    private func ensure(_ entry: inout Context) async throws {
        // Status also starts/restarts a stopped worker before inspecting its generation.
        _ = try await worker.exchange(NativeFrame.word(4))
        if entry.generation == worker.generation { return }
        if opens >= 64 { await worker.close(); opens = 0 }
        _ = try await worker.exchange(NativeFrame.word(3))
        let response = try await worker.exchange(NativeFrame.word(1) + NativeFrame.string(entry.adam) + NativeFrame.string(entry.uri))
        opens += 1
        let handle = try NativeFrame.readWord(response, at: 0), size = try NativeFrame.readWord(response, at: 4)
        guard (1...64).contains(handle), size > 0, size == response.count - 8,
              let content = String(data: response.dropFirst(8), encoding: .utf8) else { throw NativeWrapperError.invalidResponse }
        entry.generation = worker.generation; entry.handle = handle; entry.content = content
    }
    @_spi(Testing) public nonisolated static func sampleSizes(_ data: Data) throws -> [Int] {
        guard data.count <= NativeWrapperLimits.maximumBody else { throw NativeWrapperError.oversized }
        let count = try NativeFrame.readWord(data, at: 0)
        guard (1...8192).contains(count) else { throw NativeWrapperError.invalidResponse }
        var offset = 4, sizes: [Int] = []; sizes.reserveCapacity(count)
        for _ in 0..<count {
            let size = try NativeFrame.readWord(data, at: offset); offset += 4
            guard (1...4 * 1024 * 1024).contains(size), size <= data.count - offset else { throw NativeWrapperError.invalidResponse }
            sizes.append(size); offset += size
        }
        guard offset == data.count else { throw NativeWrapperError.invalidResponse }; return sizes
    }
    /// Exclude streaming while the SDK account files are being changed.
    public func beginAccountOperation() async throws {
        guard !accountOperation else { throw NativeWrapperError.busy }
        accountOperation = true
        do { try await gate.acquire(); await worker.close(); await gate.release() }
        catch { accountOperation = false; throw error }
    }
    public func endAccountOperation() async {
        tokens = [:]; tokenTime = 0; contexts = [:]; opens = 0
        await web.reset(); identity.advance(); accountOperation = false
    }
    public func close() async { await worker.close() }
    public func importAccount(from source: URL) async throws {
        try await beginAccountOperation()
        do {
            let destination = configuration.profile
            try await Task.detached(priority: .userInitiated) { try NativeProfileImporter.replace(from: source, destination: destination) }.value
            await endAccountOperation()
        } catch { await endAccountOperation(); throw error }
    }
}
