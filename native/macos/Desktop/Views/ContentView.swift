import SwiftUI

struct ContentView: View {
    @Bindable var model: WrapperViewModel

    var body: some View {
        NavigationSplitView {
            List(selection: $model.page) {
                ForEach(WorkspacePage.allCases) { page in
                    Label(page.title, systemImage: page.symbol).tag(page)
                }
            }
            .navigationSplitViewColumnWidth(min: 190, ideal: 205, max: 260)
            .safeAreaInset(edge: .bottom) {
                HStack(spacing: 8) {
                    Circle().fill(model.phase == .running ? Color.green : Color.secondary).frame(width: 7, height: 7)
                    Text(model.phase.rawValue).font(.callout)
                    Spacer()
                }
                .foregroundStyle(.secondary)
                .padding(16)
            }
        } detail: {
            Group {
                switch model.page {
                case .server: ServerView(model: model)
                case .checks: ChecksView(model: model)
                }
            }
            .navigationTitle(model.page.title)
        }
        .frame(minWidth: 800, minHeight: 540)
        .task { await model.startServer() }
    }
}
