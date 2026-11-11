// Static triangle-mesh collision of an arena, in the form the game's Bullet holds it:
// btBvhTriangleMeshShape with its quantized BVH and btTriangleInfoMap. The data itself is
// extracted from the game (tools/hook/dump_collision.py) and is never part of the repo; this
// file only describes it and implements the sphere-vs-mesh narrow phase on top of it.
// See arena_mesh.cpp for the evidence behind each step.
#pragma once

#include "sim/contact.h"
#include "sim/vec.h"

#include <cstdint>

namespace rlios::sim {

// btTriangleInfo: flags bit 0/1/2 = edge V0V1/V1V2/V2V0 is convex; then the three edge angles.
// flags == -1 marks a triangle that has no entry in the info map.
struct MeshTriInfo {
    int32_t flags;
    float edgeAngle[3];
};

// btQuantizedBvhNode. escapeOrTriangle >= 0: leaf, triangle index (part id in the top bits);
// < 0: internal node, minus the number of nodes to skip when the box test fails.
struct BvhNode {
    uint16_t qmin[3];
    uint16_t qmax[3];
    int32_t escapeOrTriangle;
};

struct ArenaMesh {
    Transform xf; // world transform of the collision object
    Vec3 bvhMin, bvhMax, bvhQuant;
    int32_t numTriangles, numVertices, numNodes;
    const uint32_t* indices;  // 3 per triangle
    const float* vertices;    // 3 per vertex, mesh space
    const BvhNode* nodes;     // depth-first order, as built by the game
    const MeshTriInfo* info;  // one per triangle
};

inline constexpr int kMaxArenaMeshes = 16;

struct ArenaCollision {
    const ArenaMesh* meshes; // Bullet world object order
    int32_t numMeshes;       // <= kMaxArenaMeshes
};

inline void meshTriangle(const ArenaMesh& m, int32_t tri, Vec3 v[3]) {
    for (int k = 0; k < 3; ++k) {
        const float* p = m.vertices + 3 * static_cast<size_t>(m.indices[3 * tri + k]);
        v[k] = {p[0], p[1], p[2]};
    }
}

// btBvhTriangleMeshShape::processAllTriangles for a box in mesh space: calls fn(triangleIndex)
// for every BVH leaf whose quantized box overlaps, in the game's traversal order.
template <class F>
void forEachOverlappingLeaf(const ArenaMesh& m, const Vec3& aabbMin, const Vec3& aabbMax, F&& fn);

// SphereTriangleDetector::collide. centre is in triangle (mesh) space. Returns false for no
// contact; otherwise the contact point on the triangle, the normal and the (negative) depth.
bool sphereTriangle(const Vec3 v[3], const Vec3& centre, float radius, float breakingThreshold,
                    Vec3& point, Vec3& normal, float& depth);

// btAdjustInternalEdgeContacts: corrects the normal of a contact near a triangle edge so the
// ball does not catch on internal edges of the mesh. Rewrites normal, worldB and localB.
void adjustInternalEdge(ContactPoint& pt, const Transform& meshXf, const Vec3 v[3],
                        const MeshTriInfo& info);

// ---- implementation of the template ----------------------------------------------------------

// btQuantizedBvh::reportAabbOverlappingNodex (RVA 0x27BC70), quantized path. Every Stadium_P
// mesh has m_traversalMode 0 (measured, bvh +0xD0): walkStacklessQuantizedTree (RVA 0x27FD80)
// over all nodes.
template <class F>
void forEachOverlappingLeaf(const ArenaMesh& m, const Vec3& aabbMin, const Vec3& aabbMax, F&& fn) {
    const float lo[3] = {m.bvhMin.x, m.bvhMin.y, m.bvhMin.z};
    const float hi[3] = {m.bvhMax.x, m.bvhMax.y, m.bvhMax.z};
    const float quant[3] = {m.bvhQuant.x, m.bvhQuant.y, m.bvhQuant.z};
    const float bmin[3] = {aabbMin.x, aabbMin.y, aabbMin.z};
    const float bmax[3] = {aabbMax.x, aabbMax.y, aabbMax.z};
    uint16_t qmin[3], qmax[3];
    for (int a = 0; a < 3; ++a) {
        // quantizeWithClamp: clamp to the tree's box, truncate, round min down to even and max
        // up to odd.
        float v = bmin[a];
        if (v < lo[a])
            v = lo[a];
        if (hi[a] < v)
            v = hi[a];
        qmin[a] = static_cast<uint16_t>(
            static_cast<uint16_t>(static_cast<int32_t>((v - lo[a]) * quant[a])) & 0xfffeu);
        v = bmax[a];
        if (v < lo[a])
            v = lo[a];
        if (hi[a] < v)
            v = hi[a];
        qmax[a] = static_cast<uint16_t>(
            static_cast<uint16_t>(static_cast<int32_t>((v - lo[a]) * quant[a] + 1.0f)) | 1u);
    }
    int32_t cur = 0;
    while (cur < m.numNodes) {
        const BvhNode& n = m.nodes[cur];
        const bool overlap = qmin[0] <= n.qmax[0] && n.qmin[0] <= qmax[0] && qmin[2] <= n.qmax[2] &&
                             n.qmin[2] <= qmax[2] && qmin[1] <= n.qmax[1] && n.qmin[1] <= qmax[1];
        const bool leaf = n.escapeOrTriangle >= 0;
        if (leaf && overlap)
            fn(n.escapeOrTriangle & 0x1fffff);
        if (overlap || leaf)
            ++cur;
        else
            cur += -n.escapeOrTriangle;
    }
}

} // namespace rlios::sim
