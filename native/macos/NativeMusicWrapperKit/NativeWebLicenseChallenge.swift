import Foundation

/// A challenge belongs to the DRM client that created it; the wrapper only
/// exchanges it with Apple using the scene's verified native account.
public struct NativeWebLicenseChallenge: Equatable, Sendable {
    public enum DRM: String, CaseIterable, Identifiable, Sendable {
        case widevine = "wv", playReady = "pr"
        public var id: Self { self }
        public var title: String { self == .widevine ? "Widevine" : "PlayReady" }
        public var keySystem: String { self == .widevine ? "com.widevine.alpha" : "com.microsoft.playready" }
    }

    public var challenge: String
    public var uri: String
    public var drm: DRM

    public init(challenge: String = "", uri: String = "", drm: DRM = .widevine) {
        self.challenge = challenge
        self.uri = uri
        self.drm = drm
    }

    public func validate() throws {
        guard !challenge.isEmpty, challenge.utf8.count <= 48 * 1024,
              let decoded = Data(base64Encoded: challenge), !decoded.isEmpty,
              !uri.isEmpty, uri.utf8.count <= 8192,
              !uri.utf8.contains(where: { $0 < 32 || $0 == 127 }) else {
            throw NativeWrapperError.message("Enter a valid Base64 license challenge and key URI.")
        }
    }
}
