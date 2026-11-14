import Foundation

public struct NativeRuntimeConfiguration: Sendable {
    public let binary: URL
    public let assets: URL
    public let profile: URL

    public init(binary: URL, assets: URL, profile: URL) {
        self.binary = binary
        self.assets = assets
        self.profile = profile
    }

    /// The binary package's embed script installs these two runtime resources in the host app.
    public init(profile: URL, bundle: Bundle = .main) {
        self.init(binary: bundle.bundleURL.appendingPathComponent("Contents/MacOS/native-music-worker"),
                  assets: bundle.bundleURL.appendingPathComponent("Contents/Resources/NativeMusicWrapper/assets"),
                  profile: profile)
    }
    func prepareProfile() throws {
        try FileManager.default.createDirectory(at: profile, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: profile.path)
    }
    var arguments: [String] {
        ["--assets", assets.path, "--apple-profile", "--decryption-worker", "--data-dir", profile.path,
         "--owner-pid", String(ProcessInfo.processInfo.processIdentifier)]
    }
}

nonisolated final class NativeAccountIdentity: @unchecked Sendable {
    private let lock = NSLock()
    private var generation = UUID().uuidString
    var value: String { lock.withLock { generation } }
    func advance() { lock.withLock { generation = UUID().uuidString } }
}
