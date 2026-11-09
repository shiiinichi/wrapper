import SwiftUI

struct ServerView: View {
    @Bindable var model: WrapperViewModel

    var body: some View {
        Form {
            Section {
                HStack(alignment: .top, spacing: 16) {
                    Image(systemName: "music.note.square.stack.fill")
                        .font(.system(size: 40)).foregroundStyle(.tint)
                        .accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Apple Music, running natively").font(.title2.weight(.semibold))
                        Text("A local API powered by the Android ARM64 libraries on your Mac.")
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 8)
            }

            Section {
                LabeledContent("Status") {
                    HStack(spacing: 8) {
                        if model.phase == .starting || model.phase == .stopping { ProgressView().controlSize(.small) }
                        Text(model.phase.rawValue)
                            .foregroundStyle(model.phase == .running ? .green : .secondary)
                    }
                }
                TextField("Port", text: $model.portText)
                    .disabled(model.phase != .stopped || model.isAuthenticating)
                LabeledContent("Address") {
                    Text(model.baseURL).font(.system(.body, design: .monospaced)).textSelection(.enabled)
                    Button(action: model.copyURL) { Image(systemName: "doc.on.doc") }.buttonStyle(.borderless).help("Copy server address")
                }
                HStack {
                    if model.phase == .running {
                        Button("Stop Server") { Task { await model.stopServer() } }
                        Button("Test Endpoints") { model.page = .checks }
                            .buttonStyle(.borderedProminent)
                    } else {
                        Button("Start Server") { Task { await model.startServer() } }
                            .buttonStyle(.borderedProminent).disabled(!model.canStart)
                    }
                    Spacer()
                    Label("Native macOS", systemImage: "desktopcomputer").foregroundStyle(.secondary)
                }
            } header: {
                Text("Local Server")
            } footer: {
                Text("The API is available to local applications at this address. Keep this app open while using it.")
            }

            Section {
                LabeledContent("Account", value: model.accountPresent ? "Saved account available" : "Sign in to get started")
                if !model.regions.isEmpty { LabeledContent("Storefront", value: model.regions.joined(separator: ", ").uppercased()) }
                HStack {
                    Button(model.accountPresent ? "Sign In Again…" : "Sign In…") { Task { await model.signIn() } }
                        .disabled(model.isAuthenticating || model.isChecking || model.phase == .starting || model.phase == .stopping)
                    if model.isAuthenticating { ProgressView().controlSize(.small); Text("Complete the macOS sign-in dialogs.").foregroundStyle(.secondary) }
                }
            } header: {
                Text("Apple Account")
            } footer: {
                Text("Your account stays in a private profile on this Mac. Verification is requested only when Apple requires it.")
            }

            if let message = model.message {
                Section { Label(message, systemImage: "exclamationmark.triangle").foregroundStyle(.orange).textSelection(.enabled) }
            }
        }
        .formStyle(.grouped)
    }
}
