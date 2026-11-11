import Foundation
import os

/// Process-level diagnostics: memory footprint vs limit, thermal state.
enum SystemDiagnostics {
    /// Physical footprint in bytes (the number the jetsam limit is checked against).
    static func footprintBytes() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(
            MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) { ptr in
            ptr.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return kr == KERN_SUCCESS ? info.phys_footprint : 0
    }

    /// Bytes still available before the memory limit (0 if the OS does not report it, e.g.
    /// in the Simulator).
    static func availableBytes() -> UInt64 {
        return UInt64(max(0, os_proc_available_memory()))
    }

    static func thermalName() -> String {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal: return "nominal"
        case .fair: return "fair"
        case .serious: return "serious"
        case .critical: return "critical"
        @unknown default: return "unknown"
        }
    }

    static func mb(_ b: UInt64) -> String {
        return String(format: "%.1f", Double(b) / 1_048_576.0)
    }

    static var isSimulator: Bool {
        #if targetEnvironment(simulator)
            return true
        #else
            return false
        #endif
    }

    static func snapshot() -> [String: Any] {
        let foot = footprintBytes()
        let avail = availableBytes()
        return [
            "memory_footprint_mb": Double(foot) / 1_048_576.0,
            "memory_available_mb": Double(avail) / 1_048_576.0,
            "memory_limit_mb": avail > 0 ? Double(foot + avail) / 1_048_576.0 : 0.0,
            "thermal_state": thermalName(),
            "os_version": ProcessInfo.processInfo.operatingSystemVersionString,
            "simulator": isSimulator,
            "bundle_id": Bundle.main.bundleIdentifier ?? "",
        ]
    }
}
