// Asset pack loader (formats written by tools/convert/build_pack.py; keep both in sync).
// Platform independent; the renderer uploads what this returns. Missing or invalid packs fall back
// to a built-in placeholder arena so the app always has something to draw.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rlios::io {

constexpr uint32_t kPackVersion = 1;

enum class TexFormat : uint32_t {
    RGBA8_sRGB = 1,
    ASTC4x4_sRGB = 2,
    ASTC6x6_sRGB = 3,
    ASTC8x8_sRGB = 4,
};

enum class Blend : uint32_t { Opaque = 0, Masked = 1, Translucent = 2, Additive = 3, Modulate = 4 };

enum MaterialFlags : uint32_t {
    kMatTwoSided = 1,
    kMatUnlit = 2,
    kMatPlaceholder = 4, // material was not found in the exports
    kMatSkip = 8,        // effect material without a usable texture: not drawn yet
};

struct MipLevel {
    uint32_t width = 0, height = 0;
    uint32_t offset = 0, size = 0; // into Texture::data
};

struct Texture {
    std::string path;
    // Average sRGB colour (fallback when the texture can't be used).
    uint8_t avg[4] = {128, 128, 128, 255};
    bool loaded = false;
    TexFormat format = TexFormat::RGBA8_sRGB;
    uint32_t width = 0, height = 0;
    std::vector<MipLevel> mips;
    std::vector<uint8_t> data; // whole .rtex file
};

struct Material {
    std::string name;
    int32_t texture = -1;
    float color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    Blend blend = Blend::Opaque;
    uint32_t flags = 0;
    float alphaClip = 0.333f;
};

struct Vertex {
    float pos[3]; // Unreal units, Unreal axes (X forward, Y right, Z up)
    float nrm[3];
    float uv0[2];
    float uv1[2];
};
static_assert(sizeof(Vertex) == 40, "matches RMSH vertex layout");

struct Section {
    uint32_t firstIndex = 0, indexCount = 0;
};

struct Mesh {
    std::string path;
    bool loaded = false;
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
    std::vector<Section> sections;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct Instance {
    uint32_t mesh = 0;
    uint32_t firstSlot = 0; // slots[firstSlot + section] = material index (or -1)
    // world = v.x*rows[0] + v.y*rows[1] + v.z*rows[2] + rows[3]  (row vectors, UE3)
    float rows[4][3];
    float normalRows[3][3]; // n' = n.x*normalRows[0] + ... (inverse transpose of the linear part)
    uint32_t flags = 0;     // bit0: mirrored (negative determinant)
};

struct Scene {
    std::string name;
    bool placeholder = true;
    std::string status; // human-readable load result, for diagnostics
    std::vector<Texture> textures;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    std::vector<int32_t> slots;
};

// Strict load of `<dir>/manifest.txt` and its scene. Missing textures/meshes degrade per asset
// (texture -> average colour; mesh -> its instances are dropped); a missing/invalid manifest or
// scene returns false with `err`.
bool loadPack(const std::string& dir, Scene& out, std::string& err);

// loadPack, or the placeholder arena if that fails. Never returns an empty scene.
Scene loadSceneOrPlaceholder(const std::string& dir);

// Built-in stand-in arena: floor, side walls and goal boxes at the standard field dimensions.
Scene makePlaceholderScene();

// Bytes per row of mip `m` for upload (ASTC: blocks across * 16; RGBA8: width * 4).
uint32_t bytesPerRow(TexFormat f, uint32_t width);
// ASTC block footprint (1x1 for RGBA8).
void blockSize(TexFormat f, uint32_t& bw, uint32_t& bh);

} // namespace rlios::io
