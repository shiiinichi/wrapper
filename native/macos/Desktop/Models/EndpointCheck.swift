import Foundation

enum WorkspacePage: String, CaseIterable, Identifiable {
    case server, checks
    var id: String { rawValue }
    var title: String { self == .server ? "Server" : "Endpoint Checks" }
    var symbol: String { self == .server ? "server.rack" : "checklist" }
}

enum ServerPhase: String {
    case stopped = "Stopped", starting = "Starting", running = "Running", stopping = "Stopping"
}

enum CheckState: String {
    case waiting = "Waiting", running = "Checking", passed = "Passed", limited = "Limited", failed = "Failed"
    var symbol: String {
        switch self {
        case .waiting: "circle.dotted"
        case .running: "arrow.trianglehead.2.clockwise.rotate.90"
        case .passed: "checkmark.circle.fill"
        case .limited: "exclamationmark.circle.fill"
        case .failed: "xmark.circle.fill"
        }
    }
}

struct EndpointCheck: Identifiable {
    let id: String
    let title: String
    var state: CheckState = .waiting
    var detail = ""
    var milliseconds: Double?

    var responseTime: String {
        guard let milliseconds else { return "—" }
        return milliseconds < 1000 ? String(format: "%.0f ms", milliseconds) : String(format: "%.2f s", milliseconds / 1000)
    }

    static var initial: [EndpointCheck] {
        [EndpointCheck(id: "status", title: "Account status"),
         EndpointCheck(id: "m3u8", title: "Enhanced HLS manifest"),
         EndpointCheck(id: "lyrics", title: "Word-timed lyrics"),
         EndpointCheck(id: "lyrics-line", title: "Line-timed lyrics"),
         EndpointCheck(id: "webplayback", title: "WebPlayback manifest"),
         EndpointCheck(id: "key", title: "FairPlay key"),
         EndpointCheck(id: "license", title: "DRM license", state: .limited, detail: "Requires a player-generated DRM challenge.")]
    }
}
