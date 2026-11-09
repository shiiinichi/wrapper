import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var model: WrapperViewModel?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        model?.shutdownNow()
        return .terminateNow
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct NativeMusicWrapperApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model = WrapperViewModel()

    var body: some Scene {
        WindowGroup("Native Music Wrapper", id: "main") {
            ContentView(model: model)
                .onAppear { delegate.model = model }
        }
        .defaultSize(width: 1010, height: 670)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandMenu("Server") {
                Button("Start Server") { Task { await model.startServer() } }
                    .keyboardShortcut("r", modifiers: .command).disabled(!model.canStart)
                Button("Stop Server") { Task { await model.stopServer() } }
                    .keyboardShortcut(".", modifiers: .command).disabled(model.phase != .running)
                Divider()
                Button("Sign In…") { Task { await model.signIn() } }.disabled(model.isAuthenticating || model.isChecking)
                Button("Check Endpoints") { model.page = .checks; Task { await model.runChecks() } }
                    .keyboardShortcut("t", modifiers: .command).disabled(model.phase != .running || model.isChecking)
            }
        }
    }
}
