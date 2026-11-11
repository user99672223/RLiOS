#include "io/pack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace rlios::io {
namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    uint8_t buf[1 << 16];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

// Bounds-checked little-endian reader; any overrun sets `ok = false` and returns zeros.
struct Reader {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;
    bool ok = true;

    bool take(void* dst, size_t len) {
        if (!ok || len > n - pos) {
            ok = false;
            std::memset(dst, 0, len);
            return false;
        }
        std::memcpy(dst, p + pos, len); // host is little-endian (x86-64, arm64)
        pos += len;
        return true;
    }
    uint32_t u32() {
        uint32_t v;
        take(&v, 4);
        return v;
    }
    int32_t i32() {
        int32_t v;
        take(&v, 4);
        return v;
    }
    float f32() {
        float v;
        take(&v, 4);
        return v;
    }
    bool magic(const char* m) {
        char got[4];
        take(got, 4);
        return ok && std::memcmp(got, m, 4) == 0;
    }
};

bool loadMesh(const std::string& path, Mesh& m) {
    std::vector<uint8_t> d;
    if (!readFile(path, d))
        return false;
    Reader r{d.data(), d.size()};
    if (!r.magic("RMSH") || r.u32() != 1)
        return false;
    const uint32_t nv = r.u32(), ni = r.u32(), ns = r.u32();
    for (float& v : m.boundsMin)
        v = r.f32();
    for (float& v : m.boundsMax)
        v = r.f32();
    if (!r.ok || ns != m.sections.size())
        return false;
    // Sizes must match the file exactly before allocating.
    const uint64_t need = r.pos + 8ull * ns + 40ull * nv + 4ull * ni;
    if (need != d.size())
        return false;
    for (Section& s : m.sections) {
        s.firstIndex = r.u32();
        s.indexCount = r.u32();
        if (uint64_t(s.firstIndex) + s.indexCount > ni)
            return false;
    }
    m.vertices.resize(nv);
    r.take(m.vertices.data(), 40ull * nv);
    m.indices.resize(ni);
    r.take(m.indices.data(), 4ull * ni);
    for (uint32_t i : m.indices)
        if (i >= nv)
            return false;
    m.loaded = r.ok;
    return r.ok;
}

bool loadTexture(const std::string& path, Texture& t) {
    if (!readFile(path, t.data))
        return false;
    Reader r{t.data.data(), t.data.size()};
    if (!r.magic("RTEX") || r.u32() != 1)
        return false;
    const uint32_t fmt = r.u32();
    if (fmt < 1 || fmt > 4)
        return false;
    t.format = static_cast<TexFormat>(fmt);
    t.width = r.u32();
    t.height = r.u32();
    const uint32_t nmip = r.u32();
    if (!r.ok || nmip == 0 || nmip > 16 || t.width == 0 || t.height == 0)
        return false;
    t.mips.resize(nmip);
    uint32_t bw = 1, bh = 1;
    blockSize(t.format, bw, bh);
    for (uint32_t i = 0; i < nmip; ++i) {
        MipLevel& m = t.mips[i];
        m.width = r.u32();
        m.height = r.u32();
        m.offset = r.u32();
        m.size = r.u32();
        const uint32_t ew = std::max(1u, t.width >> i), eh = std::max(1u, t.height >> i);
        const uint64_t expect = uint64_t(bytesPerRow(t.format, ew)) * ((eh + bh - 1) / bh);
        if (!r.ok || m.width != ew || m.height != eh || m.size != expect ||
            uint64_t(m.offset) + m.size > t.data.size())
            return false;
    }
    t.loaded = true;
    return true;
}

void setIdentity(Instance& in) {
    std::memset(in.rows, 0, sizeof(in.rows));
    std::memset(in.normalRows, 0, sizeof(in.normalRows));
    for (int i = 0; i < 3; ++i)
        in.rows[i][i] = in.normalRows[i][i] = 1.0f;
}

} // namespace

void blockSize(TexFormat f, uint32_t& bw, uint32_t& bh) {
    switch (f) {
    case TexFormat::ASTC4x4_sRGB:
        bw = bh = 4;
        break;
    case TexFormat::ASTC6x6_sRGB:
        bw = bh = 6;
        break;
    case TexFormat::ASTC8x8_sRGB:
        bw = bh = 8;
        break;
    default:
        bw = bh = 1;
        break;
    }
}

uint32_t bytesPerRow(TexFormat f, uint32_t width) {
    uint32_t bw = 1, bh = 1;
    blockSize(f, bw, bh);
    if (f == TexFormat::RGBA8_sRGB)
        return width * 4;
    return ((width + bw - 1) / bw) * 16;
}

bool loadPack(const std::string& dir, Scene& out, std::string& err) {
    out = Scene{};
    std::vector<uint8_t> man;
    if (!readFile(dir + "/manifest.txt", man)) {
        err = "no manifest.txt in " + dir;
        return false;
    }
    std::istringstream ms(std::string(man.begin(), man.end()));
    std::string line, scenePath;
    std::getline(ms, line);
    if (line != "rlios-assetpack " + std::to_string(kPackVersion)) {
        err = "unsupported manifest header: " + line;
        return false;
    }
    uint64_t files = 0, badFiles = 0;
    while (std::getline(ms, line)) {
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        if (kind == "scene" && scenePath.empty()) {
            ls >> out.name >> scenePath;
        } else if (kind == "file") {
            std::string sha, rel;
            uint64_t size = 0;
            ls >> sha >> size >> rel;
            ++files;
            std::FILE* f = std::fopen((dir + "/" + rel).c_str(), "rb");
            long got = -1;
            if (f) {
                std::fseek(f, 0, SEEK_END);
                got = std::ftell(f);
                std::fclose(f);
            }
            if (got < 0 || uint64_t(got) != size)
                ++badFiles;
        }
    }
    if (scenePath.empty()) {
        err = "manifest has no scene";
        return false;
    }
    std::vector<uint8_t> d;
    if (!readFile(dir + "/" + scenePath, d)) {
        err = "missing scene " + scenePath;
        return false;
    }
    Reader r{d.data(), d.size()};
    if (!r.magic("RSCN") || r.u32() != 1) {
        err = "bad scene header";
        return false;
    }
    const uint32_t ns = r.u32(), nt = r.u32(), nm = r.u32(), nme = r.u32(), ni = r.u32(),
                   nsl = r.u32();
    // Each record is at least 2 bytes (strings) / its fixed size: reject absurd counts before
    // allocating.
    if (!r.ok || uint64_t(ns) * 2 + uint64_t(nt) * 8 + uint64_t(nm) * 36 + uint64_t(nme) * 8 +
                         uint64_t(ni) * 100 + uint64_t(nsl) * 4 >
                     d.size()) {
        err = "bad scene counts";
        return false;
    }
    std::vector<std::string> strs(ns);
    for (std::string& s : strs) {
        uint16_t len = 0;
        r.take(&len, 2);
        if (!r.ok || len > d.size() - r.pos) {
            r.ok = false;
            break;
        }
        s.assign(reinterpret_cast<const char*>(d.data() + r.pos), len);
        r.pos += len;
    }
    auto str = [&](uint32_t id) { return id < strs.size() ? strs[id] : std::string(); };
    out.textures.resize(nt);
    for (Texture& t : out.textures) {
        t.path = str(r.u32());
        r.take(t.avg, 4);
    }
    out.materials.resize(nm);
    for (Material& m : out.materials) {
        m.name = str(r.u32());
        m.texture = r.i32();
        for (float& c : m.color)
            c = r.f32();
        m.blend = static_cast<Blend>(r.u32());
        m.flags = r.u32();
        m.alphaClip = r.f32();
        if (m.texture >= int32_t(nt) || m.texture < -1 || uint32_t(m.blend) > 4)
            r.ok = false;
    }
    out.meshes.resize(nme);
    for (Mesh& m : out.meshes) {
        m.path = str(r.u32());
        const uint32_t nsec = r.u32();
        if (nsec > 4096)
            r.ok = false;
        else
            m.sections.resize(nsec);
    }
    out.instances.resize(ni);
    for (Instance& in : out.instances) {
        in.mesh = r.u32();
        in.firstSlot = r.u32();
        r.take(in.rows, sizeof(in.rows));
        r.take(in.normalRows, sizeof(in.normalRows));
        in.flags = r.u32();
        if (in.mesh >= nme || uint64_t(in.firstSlot) + out.meshes[in.mesh].sections.size() > nsl)
            r.ok = false;
    }
    out.slots.resize(nsl);
    for (int32_t& s : out.slots) {
        s = r.i32();
        if (s < -1 || s >= int32_t(nm))
            r.ok = false;
    }
    if (!r.ok || r.pos != d.size()) {
        err = "corrupt scene " + scenePath;
        out = Scene{};
        return false;
    }

    uint32_t texOk = 0, meshOk = 0;
    for (Texture& t : out.textures) {
        if (loadTexture(dir + "/" + t.path, t))
            ++texOk;
        else
            t.data.clear();
    }
    for (Mesh& m : out.meshes) {
        if (loadMesh(dir + "/" + m.path, m)) {
            ++meshOk;
        } else {
            m.vertices.clear();
            m.indices.clear();
            m.loaded = false;
        }
    }
    std::vector<Instance> kept;
    kept.reserve(out.instances.size());
    for (const Instance& in : out.instances)
        if (out.meshes[in.mesh].loaded)
            kept.push_back(in);
    const size_t dropped = out.instances.size() - kept.size();
    out.instances.swap(kept);
    out.placeholder = false;
    std::ostringstream st;
    st << "pack " << out.name << ": " << out.instances.size() << " instances (" << dropped
       << " dropped), " << meshOk << "/" << nme << " meshes, " << texOk << "/" << nt
       << " textures, " << nm << " materials, " << files - badFiles << "/" << files << " files ok";
    out.status = st.str();
    return true;
}

Scene makePlaceholderScene() {
    // Standard soccar field (RLBot wiki "Useful game values"): side walls x = +-4096, back walls y
    // = +-5120, ceiling z = 2044, goal 1786 wide (posts at x = +-892.755), 642.775 high, 880 deep.
    Scene s;
    s.name = "placeholder";
    s.placeholder = true;
    Material floor;
    floor.name = "placeholder.floor";
    floor.color[0] = 0.16f;
    floor.color[1] = 0.36f;
    floor.color[2] = 0.14f;
    Material wall;
    wall.name = "placeholder.wall";
    wall.color[0] = wall.color[1] = wall.color[2] = 0.55f;
    wall.flags = kMatTwoSided;
    Material goalB = wall, goalO = wall;
    goalB.name = "placeholder.goal_blue";
    goalB.color[0] = 0.15f;
    goalB.color[1] = 0.35f;
    goalB.color[2] = 0.9f;
    goalO.name = "placeholder.goal_orange";
    goalO.color[0] = 0.95f;
    goalO.color[1] = 0.5f;
    goalO.color[2] = 0.1f;
    s.materials = {floor, wall, goalB, goalO};

    Mesh m;
    m.path = "placeholder";
    m.loaded = true;
    auto quad = [&m](const float a[3], const float b[3], const float c[3], const float d[3]) {
        float e1[3], e2[3], n[3];
        for (int i = 0; i < 3; ++i) {
            e1[i] = b[i] - a[i];
            e2[i] = d[i] - a[i];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& v : n)
            v = len > 0.0f ? v / len : 0.0f;
        const uint32_t base = uint32_t(m.vertices.size());
        const float* p[4] = {a, b, c, d};
        const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; ++k) {
            Vertex v{};
            std::memcpy(v.pos, p[k], sizeof(v.pos));
            std::memcpy(v.nrm, n, sizeof(v.nrm));
            std::memcpy(v.uv0, uv[k], sizeof(v.uv0));
            std::memcpy(v.uv1, uv[k], sizeof(v.uv1));
            m.vertices.push_back(v);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u})
            m.indices.push_back(base + i);
    };
    auto section = [&m](uint32_t first) {
        m.sections.push_back({first, uint32_t(m.indices.size()) - first});
    };
    constexpr float X = 4096.0f, Y = 5120.0f, Z = 2044.0f, GX = 892.755f, GZ = 642.775f,
                    GD = 880.0f;
    {
        const float a[3] = {-X, -Y, 0}, b[3] = {X, -Y, 0}, c[3] = {X, Y, 0}, d[3] = {-X, Y, 0};
        quad(a, b, c, d);
        section(0);
    }
    {
        const uint32_t first = uint32_t(m.indices.size());
        const float w[4][2] = {{-X, -Y}, {X, -Y}, {X, Y}, {-X, Y}};
        for (int i = 0; i < 4; ++i) {
            const float* p = w[i];
            const float* q = w[(i + 1) % 4];
            const float a[3] = {p[0], p[1], 0}, b[3] = {q[0], q[1], 0}, c[3] = {q[0], q[1], Z},
                        d[3] = {p[0], p[1], Z};
            quad(a, b, c, d);
        }
        section(first);
    }
    for (int g = 0; g < 2; ++g) {
        const uint32_t first = uint32_t(m.indices.size());
        const float sy = g == 0 ? -1.0f : 1.0f, y0 = sy * Y, y1 = sy * (Y + GD);
        const float back[4][3] = {{-GX, y1, 0}, {GX, y1, 0}, {GX, y1, GZ}, {-GX, y1, GZ}};
        quad(back[0], back[1], back[2], back[3]);
        const float l[4][3] = {{-GX, y0, 0}, {-GX, y1, 0}, {-GX, y1, GZ}, {-GX, y0, GZ}};
        quad(l[0], l[1], l[2], l[3]);
        const float rr[4][3] = {{GX, y0, 0}, {GX, y1, 0}, {GX, y1, GZ}, {GX, y0, GZ}};
        quad(rr[0], rr[1], rr[2], rr[3]);
        const float top[4][3] = {{-GX, y0, GZ}, {GX, y0, GZ}, {GX, y1, GZ}, {-GX, y1, GZ}};
        quad(top[0], top[1], top[2], top[3]);
        section(first);
    }
    m.boundsMin[0] = -X;
    m.boundsMin[1] = -(Y + GD);
    m.boundsMax[0] = X;
    m.boundsMax[1] = Y + GD;
    m.boundsMax[2] = Z;
    s.meshes.push_back(std::move(m));
    Instance in;
    setIdentity(in);
    s.instances.push_back(in);
    s.slots = {0, 1, 2, 3};
    s.status = "placeholder arena";
    return s;
}

Scene loadSceneOrPlaceholder(const std::string& dir) {
    Scene s;
    std::string err;
    if (!dir.empty() && loadPack(dir, s, err) && !s.instances.empty())
        return s;
    if (err.empty() && !dir.empty())
        err = "pack has no drawable instances";
    Scene p = makePlaceholderScene();
    p.status = "placeholder arena (" + (dir.empty() ? std::string("no pack path") : err) + ")";
    return p;
}

} // namespace rlios::io
