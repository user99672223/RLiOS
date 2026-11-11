// Loader for an arena collision file (RLCOL1, written by tools/hook/dump_collision.py from the
// running game). The file is game data: it lives outside the repo and later in the asset pack.
#pragma once

#include "sim/arena_mesh.h"

#include <string>
#include <vector>

namespace rlios::io {

// Owns the mesh data and exposes it to the sim as an ArenaCollision view.
class ArenaCollisionData {
public:
    // Returns false (and sets error) if the file is missing or malformed.
    bool load(const std::string& path, std::string& error);
    const sim::ArenaCollision& collision() const { return view_; }

private:
    struct Storage {
        std::vector<uint32_t> indices;
        std::vector<float> vertices;
        std::vector<sim::BvhNode> nodes;
        std::vector<sim::MeshTriInfo> info;
    };
    std::vector<Storage> storage_;
    std::vector<sim::ArenaMesh> meshes_;
    sim::ArenaCollision view_{nullptr, 0};
};

} // namespace rlios::io
