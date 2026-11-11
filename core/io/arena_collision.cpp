#include "io/arena_collision.h"

#include <cstdio>
#include <cstring>

namespace rlios::io {

namespace {

static_assert(sizeof(sim::BvhNode) == 16 && sizeof(sim::MeshTriInfo) == 16);

struct Reader {
    const std::vector<unsigned char>& buf;
    size_t pos = 0;
    bool ok = true;

    // Little-endian file, little-endian targets only (x86-64, arm64).
    template <class T> void read(T* out, size_t count) {
        const size_t bytes = count * sizeof(T);
        if (!ok || bytes > buf.size() - pos) {
            ok = false;
            return;
        }
        if (bytes != 0)
            std::memcpy(out, buf.data() + pos, bytes);
        pos += bytes;
    }
};

} // namespace

bool ArenaCollisionData::load(const std::string& path, std::string& error) {
    storage_.clear();
    meshes_.clear();
    view_ = {nullptr, 0};

    std::vector<unsigned char> buf;
    {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) {
            error = "cannot open " + path;
            return false;
        }
        unsigned char chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;)
            buf.insert(buf.end(), chunk, chunk + n);
        std::fclose(f);
    }

    Reader r{buf};
    char magic[8] = {};
    uint32_t count = 0;
    r.read(magic, 8);
    r.read(&count, 1);
    if (!r.ok || std::memcmp(magic, "RLCOL1\0\0", 8) != 0 ||
        count > static_cast<uint32_t>(sim::kMaxArenaMeshes)) {
        error = "not an RLCOL1 file: " + path;
        return false;
    }
    storage_.resize(count);
    meshes_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        float xf[12], bvh[9];
        uint32_t n[3] = {};
        r.read(xf, 12);
        r.read(bvh, 9);
        r.read(n, 3);
        // Triangle indices live in 21 bits of a BVH leaf.
        if (!r.ok || n[0] > 0x1fffffu || n[1] > 0x1fffffu || n[2] > 0x7fffffu) {
            error = "truncated or corrupt collision file: " + path;
            return false;
        }
        Storage& s = storage_[i];
        s.indices.resize(3 * static_cast<size_t>(n[0]));
        s.vertices.resize(3 * static_cast<size_t>(n[1]));
        s.nodes.resize(n[2]);
        s.info.resize(n[0]);
        r.read(s.indices.data(), s.indices.size());
        r.read(s.vertices.data(), s.vertices.size());
        r.read(s.nodes.data(), s.nodes.size());
        r.read(s.info.data(), s.info.size());
        bool valid = r.ok;
        for (size_t k = 0; valid && k < s.indices.size(); ++k)
            valid = s.indices[k] < n[1];
        for (size_t k = 0; valid && k < s.nodes.size(); ++k) {
            const int32_t e = s.nodes[k].escapeOrTriangle;
            // Leaves must name a triangle; internal nodes must skip forward.
            valid = e >= 0 ? static_cast<uint32_t>(e & 0x1fffff) < n[0] && (e >> 21) == 0
                           : e != INT32_MIN;
        }
        if (!valid) {
            error = "truncated or corrupt collision file: " + path;
            return false;
        }
        sim::ArenaMesh& m = meshes_[i];
        for (int row = 0; row < 3; ++row)
            m.xf.basis.r[row] = {xf[row * 3], xf[row * 3 + 1], xf[row * 3 + 2]};
        m.xf.origin = {xf[9], xf[10], xf[11]};
        m.bvhMin = {bvh[0], bvh[1], bvh[2]};
        m.bvhMax = {bvh[3], bvh[4], bvh[5]};
        m.bvhQuant = {bvh[6], bvh[7], bvh[8]};
        m.numTriangles = static_cast<int32_t>(n[0]);
        m.numVertices = static_cast<int32_t>(n[1]);
        m.numNodes = static_cast<int32_t>(n[2]);
        m.indices = s.indices.data();
        m.vertices = s.vertices.data();
        m.nodes = s.nodes.data();
        m.info = s.info.data();
    }
    if (r.pos != buf.size()) {
        error = "trailing data in collision file: " + path;
        return false;
    }
    view_ = {meshes_.data(), static_cast<int32_t>(count)};
    return true;
}

} // namespace rlios::io
