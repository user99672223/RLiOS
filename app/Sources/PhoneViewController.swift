import UIKit

/// Phone screen: diagnostics panel. With no external display, the game view is shown behind the
/// panel (overlay mode).
final class PhoneViewController: UIViewController {
    private let gameView = MetalGameView()
    private let panel = UILabel()
    private var timer: Timer?
    private var lastFrames: UInt64 = 0
    private var lastTime = Date()
    private var fps = 0.0

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor(red: 0.04, green: 0.05, blue: 0.08, alpha: 1)
        gameView.frame = view.bounds
        gameView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(gameView)

        panel.numberOfLines = 0
        panel.font = UIFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        panel.textColor = .white
        panel.backgroundColor = UIColor(white: 0, alpha: 0.6)
        panel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(panel)
        let g = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            panel.leadingAnchor.constraint(equalTo: g.leadingAnchor, constant: 8),
            panel.trailingAnchor.constraint(lessThanOrEqualTo: g.trailingAnchor, constant: -8),
            panel.topAnchor.constraint(equalTo: g.topAnchor, constant: 8),
        ])
        NotificationCenter.default.addObserver(
            self, selector: #selector(modeChanged), name: GameHost.externalChanged, object: nil)
        applyMode()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            self?.refresh()
        }
        refresh()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        timer?.invalidate()
        timer = nil
    }

    @objc private func modeChanged() { applyMode() }

    private func applyMode() {
        let external = GameHost.shared.hasExternalDisplay
        gameView.isHidden = external
        if external { gameView.stopRendering() } else { gameView.startRendering() }
    }

    private func refresh() {
        let host = GameHost.shared
        let d = host.diagnostics()
        let now = Date()
        let dt = now.timeIntervalSince(lastTime)
        if dt >= 1.0 {
            fps = Double(host.frames &- lastFrames) / dt
            lastFrames = host.frames
            lastTime = now
        }
        let foot = SystemDiagnostics.footprintBytes()
        let avail = SystemDiagnostics.availableBytes()
        let mem =
            avail > 0
            ? "\(SystemDiagnostics.mb(foot)) MB used / \(SystemDiagnostics.mb(foot + avail)) MB limit"
            : "\(SystemDiagnostics.mb(foot)) MB used / limit n/a"
        var lines: [String] = []
        lines.append("RLiOS M1 skeleton  (PLACEHOLDER sim, not RL physics)")
        lines.append(
            host.hasExternalDisplay
                ? "display: external connected (game on monitor)"
                : "display: no external, game on phone + overlay")
        lines.append(
            String(
                format: "sim: tick %llu  rate %.1f Hz  dropped %llu", d.tick, d.tick_rate_hz,
                d.dropped_ticks))
        lines.append(String(format: "state hash: %016llx", d.state_hash))
        lines.append(
            String(
                format: "frame: %.0f fps  cpu %.2f ms  gpu %.2f ms", fps, host.cpuMs, host.gpuMs))
        lines.append(host.sceneSummary)
        lines.append("mem: \(mem)")
        lines.append("thermal: \(SystemDiagnostics.thermalName())")
        lines.append("--- log ---")
        lines.append(contentsOf: Log.shared.tail(6))
        panel.text = lines.joined(separator: "\n")
    }
}
