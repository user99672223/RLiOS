import Foundation

/// The asset pack lives in <Documents>/assetpack (synced there by the deploy step / CI; never in the .ipa).
enum AssetPack {
    static func directory() -> URL {
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return docs.appendingPathComponent("assetpack", isDirectory: true)
    }

    /// Loads the scene into the renderer (placeholder arena if the pack is missing). True = real pack.
    @discardableResult
    static func load(into renderer: OpaquePointer) -> Bool {
        return directory().path.withCString { rlios_renderer_load_scene(renderer, $0) } == 1
    }

    static func info(_ renderer: OpaquePointer) -> [String: Any] {
        var i = RliosSceneInfo()
        rlios_renderer_scene_info(renderer, &i)
        let status = withUnsafeBytes(of: i.status) { buf in
            String(decoding: buf.prefix { $0 != 0 }, as: UTF8.self)
        }
        return [
            "real_pack": i.real_pack == 1,
            "instances": Int(i.instances),
            "draws": Int(i.draws),
            "triangles": Int(i.triangles),
            "textures": Int(i.textures),
            "textures_gpu": Int(i.textures_gpu),
            "astc_supported": i.astc_supported == 1,
            "status": status,
        ]
    }

    static func summary(_ renderer: OpaquePointer) -> String {
        let i = info(renderer)
        let real = (i["real_pack"] as? Bool) == true
        let draws = i["draws"] as? Int ?? 0
        let tris = i["triangles"] as? Int ?? 0
        let tex = "\(i["textures_gpu"] as? Int ?? 0)/\(i["textures"] as? Int ?? 0)"
        return "scene: \(real ? "pack" : "placeholder")  \(draws) draws  \(tris) tris  tex \(tex)"
    }
}
