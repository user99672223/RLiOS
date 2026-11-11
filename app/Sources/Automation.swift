import UIKit

/// Headless automation via launch arguments (CLAUDE.md section 10, "Automation mode").
///   --capture <dir>   render the external-display view offscreen at 2560x1440, write
///                     <Documents>/<dir>/capture.png and diagnostics.json, exit 0
///   --soak <seconds>  run sim + render for that long; exit 0 if healthy, 1 otherwise
///   --scenario/--flow not implemented yet: exit 2
enum Automation {
    private(set) static var active = false

    static func handle(arguments args: [String]) {
        func value(_ flag: String) -> String? {
            guard let i = args.firstIndex(of: flag), i + 1 < args.count else { return nil }
            return args[i + 1]
        }
        if let dir = value("--capture") {
            active = true
            run { runCapture(dir: dir) }
        } else if let s = value("--soak") {
            active = true
            run { runSoak(seconds: Double(s) ?? 10) }
        } else if args.contains("--scenario") || args.contains("--flow") {
            active = true
            let flag = args.contains("--scenario") ? "--scenario" : "--flow"
            run { notImplemented(flag) }
        }
    }

    // MARK: - helpers

    private static func run(_ body: @escaping () -> Int32) {
        DispatchQueue.global(qos: .userInitiated).async {
            let code = body()
            Log.info("automation finished, exit \(code)")
            exit(code)
        }
    }

    private static func outputDir(_ name: String) -> URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let url = name.hasPrefix("/") ? URL(fileURLWithPath: name) : docs.appendingPathComponent(name)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private static func writeJSON(_ dict: [String: Any], to url: URL) {
        var d = dict
        for (k, v) in SystemDiagnostics.snapshot() { d[k] = v }
        if let data = try? JSONSerialization.data(
            withJSONObject: d, options: [.prettyPrinted, .sortedKeys])
        {
            try? data.write(to: url, options: .atomic)
        }
    }

    private static func hex(_ h: UInt64) -> String { String(format: "%016llx", h) }

    // MARK: - modes

    static func runCapture(dir: String) -> Int32 {
        let out = outputDir(dir)
        let jsonURL = out.appendingPathComponent("diagnostics.json")
        let pngURL = out.appendingPathComponent("capture.png")
        let width = 2560, height = 1440
        var result: [String: Any] = [
            "mode": "capture", "width": width, "height": height, "clear_color_rgb": [51, 102, 204],
        ]
        guard let renderer = rlios_renderer_create() else {
            result["ok"] = false
            result["exit_code"] = 1
            result["error"] = "Metal renderer unavailable (no device or default.metallib)"
            writeJSON(result, to: jsonURL)
            return 1
        }
        defer { rlios_renderer_destroy(renderer) }
        var cfg = RliosConfig(manual_stepping: 1, max_catch_up_ticks: 0)
        guard let sim = rlios_create(&cfg) else { return 1 }
        defer { rlios_destroy(sim) }

        // Deterministic content: fixed input, fixed tick count.
        rlios_set_input(sim, 0.5, -0.25)
        rlios_step(sim, 600)
        var rs = RliosRenderState()
        _ = rlios_get_render_state(sim, rlios_now_ns(), &rs)
        var stats = RliosFrameStats()
        let ok = pngURL.path.withCString {
            rlios_renderer_capture(renderer, &rs, UInt32(width), UInt32(height), $0, &stats)
        }
        var d = RliosDiagnostics()
        rlios_get_diagnostics(sim, &d)
        result["ok"] = ok == 1
        result["exit_code"] = ok == 1 ? 0 : 1
        result["png"] = pngURL.lastPathComponent
        result["sim_tick"] = d.tick
        result["state_hash"] = hex(d.state_hash)
        result["sim_x"] = Double(rs.x)
        result["sim_y"] = Double(rs.y)
        result["render_cpu_ms"] = stats.cpu_ms
        result["render_gpu_ms"] = stats.gpu_ms
        writeJSON(result, to: jsonURL)  // written last: its presence means "done"
        Log.info("capture ok=\(ok)")
        return ok == 1 ? 0 : 1
    }

    static func runSoak(seconds: Double) -> Int32 {
        let out = outputDir("soak")
        let jsonURL = out.appendingPathComponent("soak.json")
        var result: [String: Any] = ["mode": "soak", "seconds": seconds]
        guard let renderer = rlios_renderer_create() else {
            result["ok"] = false
            result["exit_code"] = 1
            result["error"] = "Metal renderer unavailable"
            writeJSON(result, to: jsonURL)
            return 1
        }
        defer { rlios_renderer_destroy(renderer) }
        var cfg = RliosConfig(manual_stepping: 0, max_catch_up_ticks: 0)
        guard let sim = rlios_create(&cfg), rlios_start(sim) == 1 else { return 1 }
        defer { rlios_destroy(sim) }

        let footStart = SystemDiagnostics.footprintBytes()
        let t0 = rlios_now_ns()
        var frames = 0
        var sawNaN = false
        var lastTick: UInt64 = 0
        var tickStalls = 0
        var stats = RliosFrameStats()
        var rng: UInt64 = 0x9E37_79B9_7F4A_7C15
        while Double(rlios_now_ns() - t0) * 1e-9 < seconds {
            // Random inputs (xorshift) to wander the placeholder sim.
            rng ^= rng << 13
            rng ^= rng >> 7
            rng ^= rng << 17
            if frames % 30 == 0 {
                rlios_set_input(
                    sim, Float(rng & 0xFF) / 127.5 - 1, Float((rng >> 8) & 0xFF) / 127.5 - 1)
            }
            var rs = RliosRenderState()
            if rlios_get_render_state(sim, rlios_now_ns(), &rs) == 1 {
                if !rs.x.isFinite || !rs.y.isFinite || !rs.vx.isFinite || !rs.vy.isFinite {
                    sawNaN = true
                }
                if frames % 30 == 0 {
                    if frames > 0 && rs.tick == lastTick { tickStalls += 1 }
                    lastTick = rs.tick
                }
                if rlios_renderer_capture(renderer, &rs, 320, 180, nil, &stats) != 1 {
                    result["render_failed"] = true
                }
            }
            frames += 1
            usleep(16_000)
        }
        var d = RliosDiagnostics()
        rlios_get_diagnostics(sim, &d)
        let expected = seconds * 120
        let tickOK = Double(d.tick) >= expected * 0.5
        let footGrowthMB = (Double(SystemDiagnostics.footprintBytes()) - Double(footStart)) / 1_048_576
        let healthy = !sawNaN && tickOK && tickStalls == 0 && footGrowthMB < 64
            && (result["render_failed"] as? Bool) != true
        result["ok"] = healthy
        result["exit_code"] = healthy ? 0 : 1
        result["frames"] = frames
        result["sim_tick"] = d.tick
        result["expected_ticks"] = expected
        result["tick_rate_hz"] = d.tick_rate_hz
        result["dropped_ticks"] = d.dropped_ticks
        result["nan_seen"] = sawNaN
        result["tick_stalls"] = tickStalls
        result["footprint_growth_mb"] = footGrowthMB
        result["state_hash"] = hex(d.state_hash)
        writeJSON(result, to: jsonURL)
        return healthy ? 0 : 1
    }

    static func notImplemented(_ flag: String) -> Int32 {
        let msg = "\(flag) is not implemented yet"
        FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
        writeJSON(
            ["mode": flag, "ok": false, "exit_code": 2, "error": msg],
            to: outputDir("automation").appendingPathComponent("not_implemented.json"))
        return 2
    }
}
