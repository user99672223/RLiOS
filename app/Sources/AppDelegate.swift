import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
    ) -> Bool {
        Automation.handle(arguments: ProcessInfo.processInfo.arguments)
        return true
    }

    func application(
        _ application: UIApplication,
        configurationForConnecting connectingSceneSession: UISceneSession,
        options: UIScene.ConnectionOptions
    ) -> UISceneConfiguration {
        let role = connectingSceneSession.role
        let external = role == .windowExternalDisplayNonInteractive
        let cfg = UISceneConfiguration(name: external ? "External" : "Phone", sessionRole: role)
        cfg.delegateClass = external ? ExternalSceneDelegate.self : PhoneSceneDelegate.self
        return cfg
    }
}
