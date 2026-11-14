import Foundation

/// The native worker issues account tokens; Swift performs the same Apple-only
/// metadata requests as the former adapter. Tokens stay in memory.
@_spi(Testing) public actor NativeAppleWebAPI {
    private let http: any NativeHTTPFetching
    private var developer = ""
    private var developerTime = 0.0
    public init(http: any NativeHTTPFetching = NativeHTTPSClient()) { self.http = http }

    private func fetch(_ url: URL, headers: [String: String] = [:], body: Data? = nil) async throws -> Data {
        try Task.checkCancellation()
        var request = URLRequest(url: url)
        request.timeoutInterval = 25; request.cachePolicy = .reloadIgnoringLocalCacheData
        request.setValue("Music/5.7 Android/10 model/Pixel6GR1YH build/1234 (dt:66)", forHTTPHeaderField: "User-Agent")
        for (key, value) in headers { request.setValue(value, forHTTPHeaderField: key) }
        if let body {
            guard body.count <= NativeWrapperLimits.maximumMetadata else { throw NativeWrapperError.oversized }
            request.httpMethod = "POST"
            request.httpBody = body
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        let data = try await http.fetch(request)
        try Task.checkCancellation()
        guard data.count <= NativeHTTPSClient.maximumBytes else { throw NativeWrapperError.oversized }
        return data
    }
    private func webDeveloperToken() async throws -> String {
        let now = ProcessInfo.processInfo.systemUptime
        if !developer.isEmpty, now - developerTime < 86400 { return developer }
        let html = String(decoding: try await fetch(URL(string: "https://music.apple.com")!), as: UTF8.self)
        let asset = try NSRegularExpression(pattern: #"/assets/index~[^/\s"<>]+\.js"#)
        guard let match = asset.firstMatch(in: html, range: NSRange(html.startIndex..., in: html)),
              let range = Range(match.range, in: html), let url = URL(string: "https://music.apple.com" + html[range]) else {
            throw NativeWrapperError.message("Apple web token asset was not found.")
        }
        let javascript = String(decoding: try await fetch(url), as: UTF8.self)
        let pattern = try NSRegularExpression(pattern: #"eyJ[A-Za-z0-9_=-]+\.[A-Za-z0-9_=-]+\.[A-Za-z0-9_=-]+"#)
        for match in pattern.matches(in: javascript, range: NSRange(javascript.startIndex..., in: javascript)) {
            guard let range = Range(match.range, in: javascript) else { continue }
            let token = String(javascript[range]), parts = token.split(separator: ".")
            guard parts.count == 3 else { continue }
            var encoded = String(parts[1]).replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
            encoded += String(repeating: "=", count: (4 - encoded.count % 4) % 4)
            guard let data = Data(base64Encoded: encoded), let claims = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  claims["iss"] as? String == "AMPWebPlay" else { continue }
            developer = token; developerTime = now; return token
        }
        throw NativeWrapperError.message("Apple web developer token was not found.")
    }
    private func catalog(_ adam: String, tokens: [String: String], suffix: String, query: [URLQueryItem]) async throws -> Data {
        let region = tokens["storefront"] ?? ""
        guard region.utf8.count == 2, region.utf8.allSatisfy({ (65...90).contains($0) || (97...122).contains($0) }) else {
            throw NativeWrapperError.message("The native account storefront is unavailable.")
        }
        let (bearer, user) = try await credentials(tokens: tokens)
        var url = URLComponents(string: "https://amp-api.music.apple.com/v1/catalog/\(region)/songs/\(adam)\(suffix)")!
        url.queryItems = query
        return try await fetch(url.url!, headers: ["Authorization": "Bearer " + bearer, "media-user-token": user, "Origin": "https://music.apple.com"])
    }

    /// Public catalog metadata uses the web developer token and never reads an account profile.
    func artist(adam: String, storefront: String) async throws -> Data? {
        var url = URLComponents(string: "https://amp-api.music.apple.com/v1/catalog/\(storefront)/artists/\(adam)")!
        url.queryItems = [.init(name: "extend", value: "artistBio,origin,bornOrFormed,editorialArtwork,hero")]
        let token = try await webDeveloperToken()
        let data: Data
        do {
            data = try await fetch(url.url!, headers: ["Authorization": "Bearer " + token, "Origin": "https://music.apple.com"])
        } catch NativeWrapperError.httpStatus(404) {
            return nil
        }
        return try NativeArtistProfile.metadata(data, adam: adam, storefront: storefront)
    }

    private func credentials(tokens: [String: String], requireWebToken: Bool = false) async throws -> (String, String) {
        let bearer: String
        do { bearer = try await webDeveloperToken() }
        catch is CancellationError { throw CancellationError() }
        catch {
            if requireWebToken { throw error }
            bearer = tokens["developer_token"] ?? ""
        }
        let user = tokens["music_token"] ?? ""
        guard !bearer.isEmpty, !user.isEmpty,
              [bearer, user].allSatisfy({ $0.utf8.count <= 128 * 1024 && !$0.contains("\r") && !$0.contains("\n") }) else {
            throw NativeWrapperError.message("Native account tokens are unavailable. Sign in again.")
        }
        return (bearer, user)
    }

    private func playbackRequest(_ url: String, tokens: [String: String], body: [String: Any],
                                 requireWebToken: Bool = false) async throws -> [String: Any] {
        let (bearer, user) = try await credentials(tokens: tokens, requireWebToken: requireWebToken)
        let data = try await fetch(URL(string: url)!, headers: ["Authorization": "Bearer " + bearer,
            "X-Apple-Music-User-Token": user, "Origin": "https://music.apple.com"],
            body: JSONSerialization.data(withJSONObject: body))
        guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              root["errors"] == nil, root["failureType"] == nil,
              (root["status"] as? Int ?? 0) == 0, (root["errorCode"] as? Int ?? 0) == 0 else {
            throw NativeWrapperError.message("Apple rejected the playback request.")
        }
        return root
    }

    private func streamURL(_ value: Any?) -> URL? {
        guard let raw = value as? String, let url = URL(string: raw), url.scheme == "https",
              url.host?.isEmpty == false, url.user == nil, url.password == nil else { return nil }
        return url
    }

    public func webPlayback(adam: String, tokens: [String: String]) async throws -> Data {
        let root = try await playbackRequest("https://play.music.apple.com/WebObjects/MZPlay.woa/wa/webPlayback",
            tokens: tokens, body: ["salableAdamId": adam])
        guard let songs = root["songList"] as? [[String: Any]], let song = songs.first else {
            throw NativeWrapperError.message("Web playback is unavailable for this track.")
        }
        if let id = song["songId"], String(describing: id) != adam {
            throw NativeWrapperError.message("Apple returned playback assets for a different track.")
        }
        let assets = song["assets"] as? [[String: Any]] ?? []
        let hls = streamURL(song["hls-playlist-url"])
        let aac = assets.first { $0["flavor"] as? String == "28:ctrp256" && streamURL($0["URL"]) != nil }
        guard let url = hls ?? streamURL(aac?["URL"]) else {
            throw NativeWrapperError.message("Web playback is unavailable for this track.")
        }
        var result: [String: Any] = ["adamId": adam, "m3u8": url.absoluteString,
            "streamType": hls == nil ? "aac-cenc" : "hls",
            "assets": assets.compactMap { asset -> [String: String]? in
                guard let flavor = asset["flavor"] as? String, let url = streamURL(asset["URL"]) else { return nil }
                return ["flavor": flavor, "URL": url.absoluteString]
            }]
        if hls == nil { result["flavor"] = "28:ctrp256" }
        return try JSONSerialization.data(withJSONObject: result)
    }

    public func license(adam: String, challenge: NativeWebLicenseChallenge, tokens: [String: String]) async throws -> Data {
        try challenge.validate()
        let root = try await playbackRequest("https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/acquireWebPlaybackLicense",
            tokens: tokens, body: ["challenge": challenge.challenge, "uri": challenge.uri,
                "key-system": challenge.drm.keySystem, "adamId": adam, "isLibrary": false, "user-initiated": true],
            requireWebToken: true)
        guard let license = root["license"] as? String, !license.isEmpty else {
            throw NativeWrapperError.message("Apple returned no playback license.")
        }
        return try JSONSerialization.data(withJSONObject: ["adamId": adam, "license": license, "renew": root["renew-after"] ?? 0])
    }

    public func assets(adam: String, tokens: [String: String]) async throws -> Data {
        let (bearer, user) = try await credentials(tokens: tokens)
        guard !user.contains(";") else { throw NativeWrapperError.invalidResponse }
        var address = URLComponents(string: "https://amp-api.music.apple.com/v1/play/assets")!
        address.queryItems = [.init(name: "id", value: adam), .init(name: "kind", value: "song"),
            .init(name: "includeLicenseUrls", value: "true"), .init(name: "hlsEncryption", value: "CBC"),
            .init(name: "hlsProfile", value: "enhancedHls")]
        let data = try await fetch(address.url!, headers: ["Authorization": "Bearer " + bearer,
            "media-user-token": user, "Cookie": "media-user-token=" + user, "Origin": "https://music.apple.com"])
        guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any], root["errors"] == nil,
              let results = root["results"] as? [String: Any], let assets = results["assets"] as? [[String: Any]],
              let url = assets.lazy.compactMap({ self.streamURL($0["url"]) }).first else {
            throw NativeWrapperError.message("Apple returned no HLS playback asset.")
        }
        return try JSONSerialization.data(withJSONObject: ["adamId": adam, "m3u8": url.absoluteString, "streamType": "hls"])
    }
    private func attributes(_ data: Data) throws -> [String: Any] {
        guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any], root["errors"] == nil,
              root["failureType"] == nil, let songs = root["data"] as? [[String: Any]],
              let fields = songs.first?["attributes"] as? [String: Any] else {
            throw NativeWrapperError.message("Apple rejected the metadata request.")
        }
        return fields
    }
    public func manifest(adam: String, tokens: [String: String]) async throws -> Data {
        let data = try await catalog(adam, tokens: tokens, suffix: "", query: [.init(name: "extend", value: "extendedAssetUrls")])
        let fields = try attributes(data)
        if let assets = fields["extendedAssetUrls"], !(assets is NSNull), !(assets is [String: Any]) {
            throw NativeWrapperError.message("Apple returned invalid stream metadata.")
        }
        let raw = (fields["extendedAssetUrls"] as? [String: Any])?["enhancedHls"]
        if raw == nil || raw is NSNull || (raw as? String) == "" {
            // An absent enhanced rendition permits another asset lookup. Apple
            // request/authentication errors and malformed URLs still propagate.
            return try await assets(adam: adam, tokens: tokens)
        }
        guard let url = streamURL(raw) else {
            throw NativeWrapperError.message("Enhanced HLS is unavailable for this track.")
        }
        return try JSONSerialization.data(withJSONObject: ["adamId": adam, "m3u8": url.absoluteString])
    }
    public func lyrics(adam: String, tokens: [String: String], language: String, script: String, syllable: Bool) async throws -> Data {
        let data = try await catalog(adam, tokens: tokens, suffix: "/syllable-lyrics", query: [
            .init(name: "l[lyrics]", value: language), .init(name: "extend", value: "ttmlLocalizations"), .init(name: "l[script]", value: script)])
        guard let text = try attributes(data)["ttmlLocalizations"] as? String, !text.isEmpty else {
            throw NativeWrapperError.message("Lyrics were not found.")
        }
        return try JSONSerialization.data(withJSONObject: ["adamId": adam, "syllable": syllable ? "true" : "false", "lyrics": syllable ? text : Self.lineLyrics(text)])
    }
    public nonisolated static func lineLyrics(_ text: String) -> String {
        let normalized = text.replacingOccurrences(of: "itunes:timing=\"Word\"", with: "itunes:timing=\"Line\"")
        var result = normalized.replacingOccurrences(of: #"</?(?:[A-Za-z_][\w.-]*:)?span\b[^>]*>"#, with: "", options: .regularExpression)
        guard let pattern = try? NSRegularExpression(pattern: #"(<(?:p|text)\b[^>]*>)([\s\S]*?)(</(?:p|text)>)"#) else { return result }
        for match in pattern.matches(in: result, range: NSRange(result.startIndex..., in: result)).reversed() {
            guard let body = Range(match.range(at: 2), in: result) else { continue }
            let collapsed = result[body].replacingOccurrences(of: " {2,}", with: " ", options: .regularExpression)
            result.replaceSubrange(body, with: collapsed)
        }
        return result
    }
    public func reset() { developer = ""; developerTime = 0 }
}
