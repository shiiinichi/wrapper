import SwiftUI

struct ChecksView: View {
    @Bindable var model: WrapperViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Check a track").font(.title2.weight(.semibold))
                Text("Verify the local API using an Apple Music track ID. These checks do not start audio playback.")
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 12) {
                TextField("Track ID", text: $model.adamID)
                    .textFieldStyle(.roundedBorder).frame(maxWidth: 260)
                    .disabled(model.isChecking)
                    .onSubmit { Task { await model.runChecks() } }
                    .accessibilityIdentifier("track-id")
                Button("Run Checks") { Task { await model.runChecks() } }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.phase != .running || model.isChecking || model.isAuthenticating)
                    .accessibilityIdentifier("run-checks")
                if model.isChecking { ProgressView().controlSize(.small) }
                Spacer()
            }
            if model.phase != .running {
                Label("Start the server to run endpoint checks.", systemImage: "info.circle").foregroundStyle(.secondary)
            }
            Table(model.checks) {
                TableColumn("Endpoint") { check in Text(check.title).fontWeight(.medium) }.width(min: 155, ideal: 180)
                TableColumn("Result") { check in
                    Label(check.state.rawValue, systemImage: check.state.symbol)
                        .foregroundStyle(color(check.state))
                }.width(95)
                TableColumn("Response") { check in Text(check.responseTime).monospacedDigit() }.width(85)
                TableColumn("Details") { check in Text(check.detail).foregroundStyle(.secondary).lineLimit(3) }
            }
            HStack {
                if let date = model.lastChecked {
                    Text("Last checked \(date.formatted(date: .omitted, time: .shortened))").font(.callout).foregroundStyle(.secondary)
                }
                Spacer()
                Button("Show Timings", action: model.showTimings)
                Button("Copy Summary", action: model.copySummary).disabled(model.lastChecked == nil || model.isChecking)
            }
            if let message = model.message {
                Label(message, systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
            }
        }
        .padding(28)
    }

    private func color(_ state: CheckState) -> Color {
        switch state {
        case .passed: .green
        case .limited: .orange
        case .failed: .red
        case .waiting, .running: .secondary
        }
    }
}
