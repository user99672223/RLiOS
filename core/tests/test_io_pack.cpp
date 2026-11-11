// Asset pack loader: synthetic packs (valid, degraded, corrupt), placeholder fallback, and the real
// pack when present (work/assetpack or $RLIOS_ASSETPACK_DIR; never in git).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "io/pack.h"
#include "test.h"

using namespace rlios::io;

namespace {

struct Buf {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) {
        const auto* c = static_cast<const uint8_t*>(p);
        b.insert(b.end(), c, c + n);
    }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void str(const std::string& s) {
        const uint16_t n = uint16_t(s.size());
        raw(&n, 2);
        raw(s.data(), s.size());
    }
};

void writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(1);
    }
    std::fwrite(d.data(), 1, d.size(), f);
    std::fclose(f);
}

uint64_t fileSize(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
}

// Writes a pack with 2 textures (RGBA8 2x2 + 2 mips, ASTC6x6 7x7 1 mip), 2 materials, 2 meshes (one
// file missing), 3 instances (one using the missing mesh).
void writeSyntheticPack(const std::string& dir, bool corruptTexture) {
    mkdir(dir.c_str(), 0755);
    for (const char* sub : {"/scenes", "/meshes", "/textures"})
        mkdir((dir + sub).c_str(), 0755);
    Buf t1;
    t1.raw("RTEX", 4);
    for (uint32_t v : {1u, 1u, 2u, 2u, 2u})
        t1.u32(v);
    t1.u32(2), t1.u32(2), t1.u32(56), t1.u32(16);
    t1.u32(1), t1.u32(1), t1.u32(72), t1.u32(corruptTexture ? 8u : 4u);
    for (int i = 0; i < 20; ++i)
        t1.u32(0xff336699u);
    if (corruptTexture)
        t1.b.resize(t1.b.size() + 4);
    writeFile(dir + "/textures/a.rtex", t1.b);
    Buf t2;
    t2.raw("RTEX", 4);
    for (uint32_t v : {1u, 3u, 7u, 7u, 1u})
        t2.u32(v);
    t2.u32(7), t2.u32(7), t2.u32(40), t2.u32(64); // 2x2 blocks * 16 bytes
    t2.b.resize(t2.b.size() + 64, 0x5a);
    writeFile(dir + "/textures/b.rtex", t2.b);

    Buf m;
    m.raw("RMSH", 4);
    for (uint32_t v : {1u, 4u, 9u, 2u})
        m.u32(v);
    for (float v : {-1.f, -2.f, 0.f, 1.f, 2.f, 3.f})
        m.f32(v);
    m.u32(0), m.u32(3), m.u32(3), m.u32(6);
    for (int i = 0; i < 4; ++i) {
        Vertex v{};
        v.pos[0] = float(i);
        v.pos[2] = 3.0f;
        v.nrm[2] = 1.0f;
        v.uv0[0] = 0.5f * float(i);
        m.raw(&v, sizeof(v));
    }
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u, 1u, 2u, 3u})
        m.u32(i);
    writeFile(dir + "/meshes/quad.rmesh", m.b);

    Buf s;
    s.raw("RSCN", 4);
    // version, strings, textures, materials, meshes, instances, slots
    for (uint32_t v : {1u, 6u, 2u, 2u, 2u, 3u, 5u})
        s.u32(v);
    for (const char* x : {"textures/a.rtex", "textures/b.rtex", "Pkg.Mat_A", "Pkg.Mat_B",
                          "meshes/quad.rmesh", "meshes/gone.rmesh"})
        s.str(x);
    s.u32(0), s.raw("\x10\x20\x30\xff", 4);
    s.u32(1), s.raw("\x40\x50\x60\xff", 4);
    s.u32(2), s.i32(0), s.f32(1), s.f32(1), s.f32(1), s.f32(1), s.u32(0), s.u32(0), s.f32(0.333f);
    s.u32(3), s.i32(-1), s.f32(.25f), s.f32(.5f), s.f32(.75f), s.f32(1), s.u32(3), s.u32(kMatSkip),
        s.f32(0.1f);
    s.u32(4), s.u32(2);
    s.u32(5), s.u32(1);
    for (uint32_t k = 0; k < 3; ++k) {
        s.u32(k == 1 ? 1u : 0u); // mesh
        s.u32(k == 0 ? 0u : (k == 1 ? 2u : 3u));
        for (int r = 0; r < 12; ++r)
            s.f32(r % 4 == 0 ? 1.0f : (r >= 9 ? 100.0f * float(k) : 0.0f));
        for (int r = 0; r < 9; ++r)
            s.f32(r % 4 == 0 ? 1.0f : 0.0f);
        s.u32(k == 2 ? 1u : 0u);
    }
    for (int32_t v : {0, 1, -1, 1, 0})
        s.i32(v);
    writeFile(dir + "/scenes/T.scn", s.b);

    std::string man = "rlios-assetpack 1\nscene T scenes/T.scn\n";
    for (const char* rel :
         {"meshes/quad.rmesh", "scenes/T.scn", "textures/a.rtex", "textures/b.rtex"})
        man += "file 0 " + std::to_string(fileSize(dir + "/" + rel)) + " " + rel + "\n";
    man += "file 0 123 meshes/gone.rmesh\n";
    writeFile(dir + "/manifest.txt", std::vector<uint8_t>(man.begin(), man.end()));
}

std::string tempDir(const char* tag) {
    char tmpl[256];
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/rlios_pack_%s_XXXXXX", tag);
    const char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string("/tmp");
}

void testPlaceholder() {
    Scene s = loadSceneOrPlaceholder("/nonexistent/rlios/pack");
    CHECK(s.placeholder);
    CHECK_EQ(s.instances.size(), size_t(1));
    CHECK_EQ(s.meshes.size(), size_t(1));
    CHECK_EQ(s.meshes[0].sections.size(), size_t(4));
    CHECK_EQ(s.slots.size(), size_t(4));
    CHECK(s.status.find("no manifest") != std::string::npos);
    const Mesh& m = s.meshes[0];
    for (const Section& sec : m.sections)
        CHECK(sec.firstIndex + sec.indexCount <= m.indices.size());
    for (uint32_t i : m.indices)
        CHECK(i < m.vertices.size());
    CHECK_EQ(m.boundsMax[2], 2044.0f);
}

void testSynthetic() {
    const std::string dir = tempDir("ok");
    writeSyntheticPack(dir, false);
    Scene s;
    std::string err;
    CHECK(loadPack(dir, s, err));
    CHECK(!s.placeholder);
    CHECK_EQ(s.name, std::string("T"));
    CHECK_EQ(s.textures.size(), size_t(2));
    CHECK(s.textures[0].loaded && s.textures[1].loaded);
    CHECK_EQ(s.textures[0].format, TexFormat::RGBA8_sRGB);
    CHECK_EQ(s.textures[0].mips.size(), size_t(2));
    CHECK_EQ(s.textures[1].format, TexFormat::ASTC6x6_sRGB);
    CHECK_EQ(bytesPerRow(TexFormat::ASTC6x6_sRGB, 7), 32u);
    CHECK_EQ(int(s.textures[1].avg[0]), 0x40);
    CHECK_EQ(s.materials[1].blend, Blend::Additive);
    CHECK_EQ(s.materials[1].flags, uint32_t(kMatSkip));
    CHECK_EQ(s.materials[1].color[2], 0.75f);
    CHECK_EQ(s.materials[0].texture, 0);
    CHECK(s.meshes[0].loaded);
    CHECK(!s.meshes[1].loaded);
    CHECK_EQ(s.meshes[0].vertices.size(), size_t(4));
    CHECK_EQ(s.meshes[0].sections[1].firstIndex, 3u);
    CHECK_EQ(s.meshes[0].vertices[3].uv0[0], 1.5f);
    CHECK_EQ(s.instances.size(), size_t(2)); // instance on the missing mesh dropped
    CHECK_EQ(s.instances[1].rows[3][0], 200.0f);
    CHECK_EQ(s.instances[1].flags, 1u);
    CHECK_EQ(s.slots[s.instances[1].firstSlot + 1], 0);
    CHECK(s.status.find("1 dropped") != std::string::npos);
    CHECK(s.status.find("4/5 files ok") != std::string::npos);
}

void testDegradedTexture() {
    const std::string dir = tempDir("tex");
    writeSyntheticPack(dir, true);
    Scene s;
    std::string err;
    CHECK(loadPack(dir, s, err));
    CHECK(!s.textures[0].loaded); // mip size mismatch -> average colour fallback
    CHECK(s.textures[1].loaded);
}

void testCorruptScene() {
    const std::string dir = tempDir("bad");
    writeSyntheticPack(dir, false);
    std::FILE* f = std::fopen((dir + "/scenes/T.scn").c_str(), "rb+");
    CHECK(f != nullptr);
    if (f) {
        // Point instance 0 at mesh 99: out of range.
        std::fseek(
            f, 32 + (2 + 15) + (2 + 15) + (2 + 9) + (2 + 9) + (2 + 17) + (2 + 17) + 16 + 72 + 16,
            SEEK_SET);
        const uint32_t bad = 99;
        std::fwrite(&bad, 4, 1, f);
        std::fclose(f);
    }
    Scene s;
    std::string err;
    CHECK(!loadPack(dir, s, err));
    CHECK(err.find("corrupt") != std::string::npos);
    Scene p = loadSceneOrPlaceholder(dir);
    CHECK(p.placeholder);

    const std::string dir2 = tempDir("ver");
    writeSyntheticPack(dir2, false);
    const std::string man = "rlios-assetpack 999\n";
    writeFile(dir2 + "/manifest.txt", std::vector<uint8_t>(man.begin(), man.end()));
    CHECK(!loadPack(dir2, s, err));
    CHECK(err.find("unsupported") != std::string::npos);
}

int testRealPack() {
    const char* env = std::getenv("RLIOS_ASSETPACK_DIR");
    const std::string dir = env ? env : std::string(RLIOS_SOURCE_DIR "/../work/assetpack");
    if (fileSize(dir + "/manifest.txt") == 0) {
        std::puts("real pack: not present, skipped");
        return 0;
    }
    Scene s;
    std::string err;
    CHECK(loadPack(dir, s, err));
    std::printf("real pack: %s\n", s.status.c_str());
    CHECK(s.instances.size() > 400);
    size_t texLoaded = 0;
    for (const Texture& t : s.textures)
        texLoaded += t.loaded;
    CHECK_EQ(texLoaded, s.textures.size());
    for (const Instance& in : s.instances)
        CHECK(s.meshes[in.mesh].loaded);
    return 1;
}

} // namespace

int main() {
    testPlaceholder();
    testSynthetic();
    testDegradedTexture();
    testCorruptScene();
    testRealPack();
    TEST_MAIN_RETURN();
}
