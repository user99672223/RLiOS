import UIKit

/// A view backed by a CAMetalLayer, driven by a display link. All rendering goes through the
/// C++ renderer in core/render.
final class MetalGameView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }
    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
    private var link: CADisplayLink?

    /// If set, the drawable has exactly this size regardless of the view size
    /// (2560x1440 on the external display).
    var fixedDrawableSize: CGSize?

    func startRendering() {
        guard link == nil else { return }
        let l = CADisplayLink(target: self, selector: #selector(frame))
        l.preferredFramesPerSecond = 60
        l.add(to: .main, forMode: .common)
        link = l
    }

    func stopRendering() {
        link?.invalidate()
        link = nil
    }

    @objc private func frame() {
        let size: CGSize
        if let fixed = fixedDrawableSize {
            size = fixed
        } else {
            let s = window?.screen.scale ?? UIScreen.main.scale
            size = CGSize(width: (bounds.width * s).rounded(), height: (bounds.height * s).rounded())
        }
        GameHost.shared.draw(layer: metalLayer, width: Int(size.width), height: Int(size.height))
    }

    deinit { stopRendering() }
}
