import UIKit

/// Owns the sim (own thread) and the renderer. Consumers (views) read snapshots on the main
/// thread only; the sim never waits on them.
final class GameHost {
    static let shared = GameHost()
    static let externalChanged = Notification.Name("RLiOSExternalDisplayChanged")

    private(set) var sim: OpaquePointer?
    private(set) var renderer: OpaquePointer?
    private(set) var externalCount = 0

    // Latest frame stats (main thread).
    private(set) var frames: UInt64 = 0
    private(set) var cpuMs: Double = 0
    private(set) var gpuMs: Double = 0
    private(set) var sceneSummary = "scene: none"

    var hasExternalDisplay: Bool { externalCount > 0 }

    func startIfNeeded() {
        if sim != nil { return }
        var cfg = RliosConfig(manual_stepping: 0, max_catch_up_ticks: 0)
        sim = rlios_create(&cfg)
        renderer = rlios_renderer_create()
        if sim == nil { Log.info("sim create failed") }
        if renderer == nil { Log.info("Metal renderer unavailable") }
        if let r = renderer {
            let real = AssetPack.load(into: r)
            sceneSummary = AssetPack.summary(r)
            Log.info(real ? "asset pack loaded" : "no asset pack: placeholder arena")
        }
        if sim != nil, rlios_start(sim) == 1 { Log.info("sim thread started (120 Hz)") }
    }

    func externalConnected() {
        externalCount += 1
        Log.info("external display connected")
        NotificationCenter.default.post(name: GameHost.externalChanged, object: nil)
    }

    func externalDisconnected() {
        externalCount = max(0, externalCount - 1)
        Log.info("external display disconnected")
        NotificationCenter.default.post(name: GameHost.externalChanged, object: nil)
    }

    func diagnostics() -> RliosDiagnostics {
        var d = RliosDiagnostics()
        if let sim = sim { rlios_get_diagnostics(sim, &d) }
        return d
    }

    /// Draw one frame of the current interpolated snapshot into `layer`.
    func draw(layer: CAMetalLayer, width: Int, height: Int) {
        guard let sim = sim, let renderer = renderer, width > 0, height > 0 else { return }
        var rs = RliosRenderState()
        guard rlios_get_render_state(sim, rlios_now_ns(), &rs) == 1 else { return }
        var stats = RliosFrameStats()
        let ptr = Unmanaged.passUnretained(layer).toOpaque()
        if rlios_renderer_draw_layer(renderer, ptr, UInt32(width), UInt32(height), &rs, &stats) == 1 {
            frames = stats.frames
            cpuMs = cpuMs == 0 ? stats.cpu_ms : cpuMs * 0.9 + stats.cpu_ms * 0.1
            gpuMs = stats.gpu_ms
        }
    }
}
