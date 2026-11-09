import AppKit
import Foundation
import Observation

@MainActor @Observable
final class WrapperViewModel {
    var page: WorkspacePage = .server
    var phase: ServerPhase = .stopped
    var portText = String(UserDefaults.standard.integer(forKey: "nativeServerPort").nonzero ?? 12341)
    var adamID = "1552791761"
    var accountPresent = false
    var regions: [String] = []
    var message: String?
    var isAuthenticating = false
    var isChecking = false
    var checks = EndpointCheck.initial
    var lastChecked: Date?
    private let runtime = RuntimeConfiguration.resolve()
    private var process: Process?
    private var signInProcess: Process?
    private var log: FileHandle?
    private var client: EndpointClient?
    private var generation = UUID()
    var baseURL: String { "http://127.0.0.1:\(portText)" }
    var canStart: Bool { phase == .stopped && !isAuthenticating }

    func startServer() async {
        guard phase == .stopped, let port = Int(portText), (1...65535).contains(port) else {
            if phase == .stopped { message = "Enter a port between 1 and 65535." }
            return
        }
        phase = .starting
        message = nil
        let instance = UUID().uuidString
        let candidate = runtime.server(port: port, instance: instance)
        let connection = EndpointClient(port: port)
        do {
            try runtime.prepareProfile()
            let handle = try runtime.logFile()
            log = handle
            candidate.standardOutput = handle
            candidate.standardError = handle
            candidate.terminationHandler = { [weak self] process in
                Task { @MainActor in
                    guard let self, self.process === process, self.phase == .running else { return }
                    self.client?.invalidate()
                    self.client = nil
                    self.process = nil
                    self.phase = .stopped
                    self.isChecking = false
                    self.generation = UUID()
                    self.message = "The native server stopped. You can start it again."
                }
            }
            try candidate.run()
            process = candidate
            for _ in 0..<35 {
                guard phase == .starting, candidate.isRunning else { break }
                if let status = try? await connection.get("status"), status["server_instance"] as? String == instance {
                    accountPresent = status["account_present"] as? Bool ?? false
                    regions = status["regions"] as? [String] ?? []
                    client = connection
                    phase = .running
                    UserDefaults.standard.set(port, forKey: "nativeServerPort")
                    return
                }
                try await Task.sleep(for: .milliseconds(250))
            }
            throw WrapperError(message: "The native server could not start on port \(port). Check whether another process is using it.")
        } catch {
            if candidate.isRunning { candidate.terminate() }
            process = nil
            connection.invalidate()
            try? log?.close()
            log = nil
            phase = .stopped
            message = error.localizedDescription
        }
    }

    func stopServer() async {
        generation = UUID()
        isChecking = false
        client?.invalidate()
        client = nil
        guard let owned = process else { phase = .stopped; return }
        phase = .stopping
        if owned.isRunning { owned.terminate() }
        for _ in 0..<30 where owned.isRunning { try? await Task.sleep(for: .milliseconds(100)) }
        if owned.isRunning { kill(owned.processIdentifier, SIGKILL) }
        process = nil
        try? log?.close()
        log = nil
        phase = .stopped
    }

    func shutdownNow() {
        generation = UUID()
        client?.invalidate()
        if let process, process.isRunning { process.terminate() }
        if let signInProcess, signInProcess.isRunning { signInProcess.terminate() }
    }

    func signIn() async {
        guard !isAuthenticating else { return }
        isAuthenticating = true
        await stopServer()
        message = nil
        do {
            try runtime.prepareProfile()
            let signer = runtime.signInProcess()
            signInProcess = signer
            let code: Int32 = try await withCheckedThrowingContinuation { continuation in
                signer.terminationHandler = { process in continuation.resume(returning: process.terminationStatus) }
                do { try signer.run() }
                catch { continuation.resume(throwing: error) }
            }
            signInProcess = nil
            if code != 0 { message = "Sign-in did not complete. You can try again using the macOS dialogs." }
        } catch { message = "The sign-in process could not be started." }
        isAuthenticating = false
        let signInMessage = message
        await startServer()
        if let signInMessage { message = signInMessage }
    }

    func runChecks() async {
        guard phase == .running, !isChecking, let client,
              !adamID.isEmpty, adamID.allSatisfy(\.isNumber), UInt64(adamID) != nil else {
            message = "Enter a numeric Apple Music track ID."
            return
        }
        let run = UUID()
        generation = run
        let track = adamID
        checks = EndpointCheck.initial
        isChecking = true
        message = nil
        var keyURI: String?
        for index in checks.indices where checks[index].id != "license" {
            guard generation == run else { return }
            checks[index].state = .running
            client.resetResponseTiming()
            do {
                let id = checks[index].id
                let detail: String
                switch id {
                case "status":
                    let status = try await client.get("status")
                    accountPresent = status["account_present"] as? Bool ?? false
                    regions = status["regions"] as? [String] ?? []
                    guard accountPresent else { throw WrapperError(message: "Sign in to test track requests.") }
                    detail = "Saved account available · " + regions.joined(separator: ", ").uppercased()
                case "m3u8", "webplayback":
                    let result = try await client.get(id, ["adamId": track])
                    guard let url = result["m3u8"] as? String else { throw WrapperError(message: "No manifest was returned.") }
                    let key = try await client.manifest(url)
                    if id == "m3u8" { keyURI = key }
                    detail = "Valid HLS playlist"
                case "lyrics", "lyrics-line":
                    let result = try await client.get("lyrics", ["adamId": track, "syllable": id == "lyrics" ? "1" : "0"])
                    guard let lyrics = result["lyrics"] as? String, !lyrics.isEmpty else { throw WrapperError(message: "No lyrics were returned.") }
                    detail = "Lyrics returned · \(lyrics.utf8.count.formatted()) bytes"
                case "key":
                    guard let keyURI else { throw WrapperError(message: "A track key URI is needed from the enhanced manifest.") }
                    let result = try await client.get("key", ["adamId": track, "uri": keyURI])
                    guard let key = result["contentKey"] as? String, !key.isEmpty else { throw WrapperError(message: "No key was returned.") }
                    if result["nativeDecryptionVersion"] as? Int == 1,
                       let context = result["nativeContext"] as? String, !context.isEmpty {
                        detail = "Key retrieved; native sample decryption available."
                    } else {
                        detail = "Key retrieved; sample decryption is unavailable."
                        guard generation == run else { return }
                        checks[index].state = .limited
                        checks[index].detail = detail
                        checks[index].milliseconds = client.lastResponseMilliseconds
                        continue
                    }
                default: continue
                }
                guard generation == run else { return }
                checks[index].state = .passed
                checks[index].detail = detail
            } catch {
                guard generation == run else { return }
                checks[index].state = .failed
                checks[index].detail = error.localizedDescription
            }
            checks[index].milliseconds = client.lastResponseMilliseconds
        }
        guard generation == run else { return }
        isChecking = false
        lastChecked = .now
    }

    func copyURL() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(baseURL, forType: .string)
    }

    func copySummary() {
        let text = "Native Music Wrapper · \(adamID)\n" + checks.map { "\($0.title): \($0.state.rawValue) · \($0.responseTime) — \($0.detail)" }.joined(separator: "\n")
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    func showTimings() {
        NSWorkspace.shared.activateFileViewerSelecting([runtime.profile.appendingPathComponent("http-timings.jsonl")])
    }
}

private extension Int {
    var nonzero: Int? { self == 0 ? nil : self }
}
