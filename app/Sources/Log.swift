import Foundation

/// Tiny thread-safe ring buffer of log lines, shown in the phone diagnostics panel.
final class Log {
    static let shared = Log()
    private let lock = NSLock()
    private var lines: [String] = []
    private let maxLines = 64

    static func info(_ message: String) {
        NSLog("RLiOS: %@", message)
        shared.append(message)
    }

    private func append(_ message: String) {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        let line = "\(f.string(from: Date())) \(message)"
        lock.lock()
        lines.append(line)
        if lines.count > maxLines { lines.removeFirst(lines.count - maxLines) }
        lock.unlock()
    }

    func tail(_ n: Int) -> [String] {
        lock.lock()
        defer { lock.unlock() }
        return Array(lines.suffix(n))
    }
}
