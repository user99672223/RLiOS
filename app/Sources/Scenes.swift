import UIKit

final class PhoneSceneDelegate: NSObject, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(
        _ scene: UIScene, willConnectTo session: UISceneSession,
        options connectionOptions: UIScene.ConnectionOptions
    ) {
        guard let ws = scene as? UIWindowScene else { return }
        Log.info("phone scene connected")
        if !Automation.active { GameHost.shared.startIfNeeded() }
        let w = UIWindow(windowScene: ws)
        w.rootViewController = PhoneViewController()
        w.makeKeyAndVisible()
        window = w
    }
}

/// Root of the external display: nothing but the game view, 2560x1440 drawable.
final class ExternalViewController: UIViewController {
    static let drawableSize = CGSize(width: 2560, height: 1440)
    private let gameView = MetalGameView()

    override func loadView() {
        gameView.fixedDrawableSize = ExternalViewController.drawableSize
        gameView.backgroundColor = .black
        view = gameView
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        gameView.startRendering()
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        gameView.stopRendering()
    }
}

final class ExternalSceneDelegate: NSObject, UIWindowSceneDelegate {
    var window: UIWindow?

    func scene(
        _ scene: UIScene, willConnectTo session: UISceneSession,
        options connectionOptions: UIScene.ConnectionOptions
    ) {
        guard let ws = scene as? UIWindowScene else { return }
        if !Automation.active { GameHost.shared.startIfNeeded() }
        let w = UIWindow(windowScene: ws)
        w.rootViewController = ExternalViewController()
        w.isHidden = false
        window = w
        GameHost.shared.externalConnected()
    }

    func sceneDidDisconnect(_ scene: UIScene) {
        window = nil
        GameHost.shared.externalDisconnected()
    }
}
