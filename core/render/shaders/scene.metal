// Arena shaders: base colour texture x material colour, simple sun + ambient + distance fog. A stand-in until
// the real material ports (CLAUDE.md section 6); geometry, transforms and textures come straight from the pack.
// Layouts must match core/render/renderer.mm (PackedVertex = io::Vertex, InstanceGPU, MaterialGPU, SceneUniforms).
#include <metal_stdlib>
using namespace metal;

struct PackedVertex {
    packed_float3 pos; // Unreal units / axes
    packed_float3 nrm;
    packed_float2 uv0;
    packed_float2 uv1;
};

struct InstanceGPU {
    float4 r0, r1, r2, r3; // world = p.x*r0 + p.y*r1 + p.z*r2 + r3 (row vectors)
    float4 n0, n1, n2;     // normal rows (inverse transpose)
};

struct MaterialGPU {
    float4 color;    // linear
    uint mode;       // 0 opaque, 1 masked, 2 translucent, 3 additive, 4 modulate
    uint flags;      // bit0 two-sided, bit1 unlit
    float alphaClip;
    float intensity; // additive / translucent strength
};

struct SceneUniforms {
    float4x4 viewProj;
    float4 cameraPos;
    float4 sunDir; // towards the sun, normalized
    float4 sunColor;
    float4 ambient;
    float4 fogColor; // rgb, a = density per uu
};

struct SceneOut {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float2 uv0;
};

vertex SceneOut scene_vs(uint vid [[vertex_id]],
                         const device PackedVertex* verts [[buffer(0)]],
                         constant InstanceGPU& inst [[buffer(1)]],
                         constant SceneUniforms& u [[buffer(2)]]) {
    const PackedVertex v = verts[vid];
    const float3 p = float3(v.pos);
    const float3 n = float3(v.nrm);
    SceneOut o;
    o.worldPos = p.x * inst.r0.xyz + p.y * inst.r1.xyz + p.z * inst.r2.xyz + inst.r3.xyz;
    o.normal = n.x * inst.n0.xyz + n.y * inst.n1.xyz + n.z * inst.n2.xyz;
    o.uv0 = float2(v.uv0);
    o.position = u.viewProj * float4(o.worldPos, 1.0);
    return o;
}

static float3 linearToSrgb(float3 c) {
    c = saturate(c);
    return select(1.055 * pow(c, float3(1.0 / 2.4)) - 0.055, c * 12.92, c <= 0.0031308);
}

fragment float4 scene_fs(SceneOut in [[stage_in]],
                         constant MaterialGPU& m [[buffer(0)]],
                         constant SceneUniforms& u [[buffer(1)]],
                         texture2d<float> tex [[texture(0)]],
                         sampler smp [[sampler(0)]]) {
    const float4 t = tex.sample(smp, in.uv0);
    const float4 base = t * m.color;
    if (m.mode == 1u && base.a < m.alphaClip)
        discard_fragment();
    float3 rgb = base.rgb;
    if ((m.flags & 2u) == 0u) {
        const float3 n = normalize(in.normal);
        const float ndl = abs(dot(n, u.sunDir.xyz)); // no back-face darkening yet
        rgb *= u.ambient.rgb + u.sunColor.rgb * ndl;
    }
    const float dist = length(in.worldPos - u.cameraPos.xyz);
    const float fog = 1.0 - exp(-dist * u.fogColor.a);
    if (m.mode == 3u) {
        // Additive (blend one, one): premultiplied by alpha, scaled, faded with distance fog.
        return float4(linearToSrgb(rgb * base.a * m.intensity * (1.0 - fog)), 0.0);
    }
    rgb = mix(rgb, u.fogColor.rgb, fog);
    const float a = m.mode == 2u ? saturate(base.a * m.intensity) : 1.0;
    return float4(linearToSrgb(rgb), a);
}
