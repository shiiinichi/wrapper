import Foundation

/// Serialize SDK operations across actor suspension points, including the
/// reset/open pair. Bound both admission and time spent waiting in the queue.
@_spi(Testing) public actor NativeRequestGate {
    public init() {}
    private var occupied = false
    private var order: [UUID] = []
    private var waiters: [UUID: NativeRequestCompletion<Void>] = [:]
    public var waitingCount: Int { order.count }

    public func acquire() async throws {
        try Task.checkCancellation()
        let id = UUID(), pending = NativeRequestCompletion<Void>()
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                guard pending.install(continuation) else { return }
                if !occupied {
                    occupied = true; pending.finish(.success(()))
                } else if order.count < 7 {
                    order.append(id); waiters[id] = pending
                    Task { [weak self] in
                        try? await Task.sleep(for: .seconds(5))
                        await self?.remove(id, error: NativeWrapperError.busy)
                    }
                } else { pending.finish(.failure(NativeWrapperError.busy)) }
            }
        } onCancel: {
            Task { await self.remove(id, error: CancellationError()) }
        }
    }
    private func remove(_ id: UUID, error: Error) {
        guard let pending = waiters.removeValue(forKey: id) else { return }
        order.removeAll { $0 == id }; pending.finish(.failure(error))
    }
    public func release() {
        while !order.isEmpty {
            let id = order.removeFirst()
            if let next = waiters.removeValue(forKey: id) { next.finish(.success(())); return }
        }
        occupied = false
    }
}
