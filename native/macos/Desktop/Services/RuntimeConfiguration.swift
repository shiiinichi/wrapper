import AppKit
import Foundation

struct RuntimeConfiguration {
    let directory: URL
    let binary: URL
    let assets: URL
    let profile: URL

    static func resolve() -> RuntimeConfiguration {
        let resources = Bundle.main.resourceURL!.appendingPathComponent("Runtime", isDirectory: true)
        let arguments = ProcessInfo.processInfo.arguments
        var profilePath = UserDefaults.standard.string(forKey: "nativeProfilePath")
        if let index = arguments.firstIndex(of: "--profile"), arguments.indices.contains(index + 1) {
            profilePath = arguments[index + 1]
            UserDefaults.standard.set(profilePath, forKey: "nativeProfilePath")
        }
        let fallback = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Native Music Wrapper", isDirectory: true)
        return RuntimeConfiguration(directory: resources,
                                    binary: Bundle.main.executableURL!.deletingLastPathComponent().appendingPathComponent("wrapper-native-poc"),
                                    assets: resources.appendingPathComponent("assets", isDirectory: true),
                                    profile: profilePath.map { URL(fileURLWithPath: $0, isDirectory: true) } ?? fallback)
    }

    func prepareProfile() throws {
        try FileManager.default.createDirectory(at: profile, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        try FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: profile.path)
    }

    func server(port: Int, instance: String) -> Process {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = ["-B", directory.appendingPathComponent("http_server.py").path,
                             "--binary", binary.path, "--assets", assets.path, "--data-dir", profile.path,
                             "--port", String(port), "--instance-id", instance,
                             "--owner-pid", String(ProcessInfo.processInfo.processIdentifier)]
        return process
    }

    func logFile() throws -> FileHandle {
        let url = profile.appendingPathComponent("native-http.log")
        if !FileManager.default.fileExists(atPath: url.path) {
            FileManager.default.createFile(atPath: url.path, contents: nil, attributes: [.posixPermissions: 0o600])
        }
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
        let handle = try FileHandle(forWritingTo: url)
        try handle.seekToEnd()
        return handle
    }

    func signInProcess() -> Process {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = ["-B", directory.appendingPathComponent("sign_in.py").path, "--binary", binary.path,
                             "--assets", assets.path, "--data-dir", profile.path,
                             "--owner-pid", String(ProcessInfo.processInfo.processIdentifier)]
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        return process
    }
}
