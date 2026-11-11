// Milestone 1 placeholder shader: draws the sim point as a disc plus the sim bounds outline over
// the cleared frame. Alpha-blended so untouched pixels keep the exact clear color.
#include <metal_stdlib>
using namespace metal;

struct FrameUniforms {
    float2 center;  // sim-space position (interpolated)
    float aspect;   // width / height
    float radius;   // sim-space disc radius
    float4 discColor;
};

struct VertexOut {
    float4 position [[position]];
    float2 ndc;
};

vertex VertexOut rlios_vs(uint vid [[vertex_id]]) {
    // Fullscreen triangle.
    const float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));
    VertexOut out;
    out.ndc = p * 2.0 - 1.0;
    out.position = float4(out.ndc, 0.0, 1.0);
    return out;
}

fragment float4 rlios_fs(VertexOut in [[stage_in]], constant FrameUniforms& u [[buffer(0)]]) {
    // Sim box [-1,1]^2 occupies 90% of the screen height, square in pixels.
    const float2 w = float2(in.ndc.x * u.aspect, in.ndc.y) / 0.9;
    const float px = fwidth(w.y);

    const float r = length(w - u.center);
    const float disc = 1.0 - smoothstep(u.radius - px, u.radius + px, r);

    const float2 e = abs(abs(w) - 1.0); // distance to the box edges
    const float inside = step(max(abs(w.x), abs(w.y)), 1.0 + 3.0 * px);
    const float line = (1.0 - smoothstep(px, 2.0 * px, min(e.x, e.y))) * inside;

    const float3 lineColor = float3(0.95, 0.95, 0.95);
    const float3 rgb = (disc * u.discColor.rgb + line * (1.0 - disc) * lineColor) /
                       max(disc + line * (1.0 - disc), 1e-5);
    const float a = max(disc, line);
    return float4(rgb, a);
}
