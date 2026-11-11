// Metal renderer (Apple only). C++-style core with a C API (see api/rlios.h); Swift stays thin.
// Draws the arena scene from the asset pack (core/io/pack.h), or the placeholder arena when no pack
// is present. All pipelines are built at creation (CLAUDE.md section 6: no pipeline creation
// mid-match).
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>
#include <simd/simd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "api/rlios.h"
#include "io/pack.h"

namespace {

// Sky / clear color: bytes (51, 102, 204) in BGRA8Unorm (also the fog colour, so distant geometry
// fades to it).
constexpr double kClearR = 51.0 / 255.0;
constexpr double kClearG = 102.0 / 255.0;
constexpr double kClearB = 204.0 / 255.0;

// Must match shaders/scene.metal.
struct InstanceGPU {
    simd_float4 r0, r1, r2, r3;
    simd_float4 n0, n1, n2;
};
struct MaterialGPU {
    simd_float4 color;
    uint32_t mode;
    uint32_t flags;
    float alphaClip;
    float intensity;
};
struct SceneUniforms {
    simd_float4x4 viewProj;
    simd_float4 cameraPos;
    simd_float4 sunDir;
    simd_float4 sunColor;
    simd_float4 ambient;
    simd_float4 fogColor;
};

struct Shot {
    const char* name;
    simd_float3 eye, target;
    float hfovDeg;
};
// Fixed capture shots (Unreal units/axes). Keep in sync with tools/convert/pack_preview.py SHOTS.
const Shot kShots[] = {
    {"overview",
     {-9000.0f, -7500.0f, 4200.0f},
     {0.0f, 0.0f, 0.0f},
     80.0f}, // == captures/preview/stadium_persp
    {"behind_goal", {0.0f, -7000.0f, 1100.0f}, {0.0f, 0.0f, 200.0f}, 90.0f},
    {"sideline", {-6200.0f, 0.0f, 1500.0f}, {0.0f, 0.0f, 300.0f}, 90.0f},
    {"high", {0.0f, -1500.0f, 11000.0f}, {0.0f, 0.0f, 0.0f}, 70.0f},
};
constexpr uint32_t kShotCount = sizeof(kShots) / sizeof(kShots[0]);

double nowMs() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch())
        .count();
}

float srgbToLinear(uint8_t c) {
    const float v = float(c) / 255.0f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

MTLPixelFormat pixelFormat(rlios::io::TexFormat f) {
    switch (f) {
    case rlios::io::TexFormat::ASTC4x4_sRGB:
        return MTLPixelFormatASTC_4x4_sRGB;
    case rlios::io::TexFormat::ASTC6x6_sRGB:
        return MTLPixelFormatASTC_6x6_sRGB;
    case rlios::io::TexFormat::ASTC8x8_sRGB:
        return MTLPixelFormatASTC_8x8_sRGB;
    default:
        return MTLPixelFormatRGBA8Unorm_sRGB;
    }
}

// Left-handed, Z-up (Unreal): right = up x forward. Reversed-Z infinite projection (depth = near /
// z), so depth clears to 0 and compares "greater".
simd_float4x4 viewProjection(simd_float3 eye, simd_float3 target, float hfovDeg, float aspect) {
    const simd_float3 f = simd_normalize(target - eye);
    simd_float3 up = {0.0f, 0.0f, 1.0f};
    if (std::fabs(simd_dot(f, up)) > 0.999f)
        up = simd_make_float3(1.0f, 0.0f, 0.0f);
    const simd_float3 r = simd_normalize(simd_cross(up, f));
    const simd_float3 u = simd_cross(f, r);
    const float fx = 1.0f / std::tan(hfovDeg * float(M_PI) / 360.0f);
    const float fy = fx * aspect;
    constexpr float kNear = 10.0f;
    const simd_float4 row0 = simd_make_float4(r * fx, -simd_dot(r, eye) * fx);
    const simd_float4 row1 = simd_make_float4(u * fy, -simd_dot(u, eye) * fy);
    const simd_float4 row2 = simd_make_float4(0.0f, 0.0f, 0.0f, kNear);
    const simd_float4 row3 = simd_make_float4(f, -simd_dot(f, eye));
    return simd_matrix_from_rows(row0, row1, row2, row3);
}

struct DrawItem {
    uint32_t instance;
    uint32_t material;
    uint32_t firstIndex; // absolute, into the shared index buffer (indices are absolute too)
    uint32_t indexCount;
    simd_float3 center; // world-space bounds centre, for back-to-front sorting
};

} // namespace

struct RliosRenderer {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeOpaque = nil, pipeTranslucent = nil, pipeAdditive = nil;
    id<MTLDepthStencilState> depthWrite = nil, depthRead = nil;
    id<MTLSamplerState> sampler = nil;
    id<MTLTexture> white = nil;
    bool astc = false;
    bool memorylessDepth = false;

    // Scene (GPU)
    id<MTLBuffer> vertices = nil, indices = nil;
    // Per-instance data goes in with setVertexBytes: constant-buffer offsets need 256-byte
    // alignment on macOS-backed GPUs (Simulator), and 112-byte records would break that.
    std::vector<InstanceGPU> instances;
    std::vector<id<MTLTexture>> textures; // per pack texture (nil -> material uses average colour)
    std::vector<MaterialGPU> materials;
    std::vector<int32_t> materialTexture;
    std::vector<DrawItem> opaque, blended;
    std::vector<uint32_t> blendedOrder;
    RliosSceneInfo info{};
    int32_t shot = -1;

    id<MTLTexture> depth = nil; // cached for the layer path
    std::atomic<double> gpuMs{0.0};
    std::atomic<uint64_t> frames{0};

    bool init() {
        device = MTLCreateSystemDefaultDevice();
        if (!device)
            return false;
        queue = [device newCommandQueue];
        id<MTLLibrary> lib = [device newDefaultLibrary];
        if (!lib)
            return false;
        if (@available(iOS 13.0, macOS 10.15, *)) {
            astc = [device supportsFamily:MTLGPUFamilyApple2];
#if !TARGET_OS_SIMULATOR
            memorylessDepth = [device supportsFamily:MTLGPUFamilyApple2];
#endif
        }
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"scene_vs"];
        d.fragmentFunction = [lib newFunctionWithName:@"scene_fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError* err = nil;
        pipeOpaque = [device newRenderPipelineStateWithDescriptor:d error:&err];
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        pipeTranslucent = [device newRenderPipelineStateWithDescriptor:d error:&err];
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        pipeAdditive = [device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pipeOpaque || !pipeTranslucent || !pipeAdditive) {
            NSLog(@"RLiOS: pipeline error: %@", err);
            return false;
        }
        MTLDepthStencilDescriptor* ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = MTLCompareFunctionGreaterEqual; // reversed Z
        ds.depthWriteEnabled = YES;
        depthWrite = [device newDepthStencilStateWithDescriptor:ds];
        ds.depthWriteEnabled = NO;
        depthRead = [device newDepthStencilStateWithDescriptor:ds];
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.mipFilter = MTLSamplerMipFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeRepeat;
        sd.tAddressMode = MTLSamplerAddressModeRepeat;
        sd.maxAnisotropy = 8;
        sampler = [device newSamplerStateWithDescriptor:sd];
        MTLTextureDescriptor* wd =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        white = [device newTextureWithDescriptor:wd];
        const uint32_t px = 0xffffffffu;
        [white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
        upload(rlios::io::makePlaceholderScene());
        return true;
    }

    id<MTLTexture> makeTexture(const rlios::io::Texture& t) {
        if (!t.loaded)
            return nil;
        if (t.format != rlios::io::TexFormat::RGBA8_sRGB && !astc)
            return nil;
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat(t.format)
                                                               width:t.width
                                                              height:t.height
                                                           mipmapped:NO];
        td.mipmapLevelCount = t.mips.size();
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        if (!tex)
            return nil;
        for (size_t i = 0; i < t.mips.size(); ++i) {
            const rlios::io::MipLevel& m = t.mips[i];
            [tex replaceRegion:MTLRegionMake2D(0, 0, m.width, m.height)
                   mipmapLevel:i
                     withBytes:t.data.data() + m.offset
                   bytesPerRow:rlios::io::bytesPerRow(t.format, m.width)];
        }
        return tex;
    }

    void upload(const rlios::io::Scene& s) {
        info = RliosSceneInfo{};
        info.real_pack = s.placeholder ? 0 : 1;
        info.astc_supported = astc ? 1 : 0;
        std::snprintf(info.status, sizeof(info.status), "%s", s.status.c_str());
        textures.clear();
        for (const rlios::io::Texture& t : s.textures) {
            id<MTLTexture> tex = makeTexture(t);
            textures.push_back(tex);
            info.textures_gpu += tex ? 1 : 0;
        }
        info.textures = uint32_t(s.textures.size());

        materials.clear();
        materialTexture.clear();
        for (const rlios::io::Material& m : s.materials) {
            MaterialGPU g{};
            g.color = simd_make_float4(m.color[0], m.color[1], m.color[2], m.color[3]);
            int32_t ti = m.texture;
            if (ti >= 0 && !textures[size_t(ti)]) {
                const uint8_t* a =
                    s.textures[size_t(ti)].avg; // texture unusable: its average colour
                g.color *= simd_make_float4(srgbToLinear(a[0]), srgbToLinear(a[1]),
                                            srgbToLinear(a[2]), float(a[3]) / 255.0f);
                ti = -1;
            }
            g.mode = uint32_t(m.blend);
            g.flags = m.flags;
            g.alphaClip = m.alphaClip;
            // Effects are drawn with their base texture only; their real shaders are far subtler.
            g.intensity = m.blend == rlios::io::Blend::Additive ? 0.35f : 1.0f;
            materials.push_back(g);
            materialTexture.push_back(ti);
        }

        // One vertex/index buffer for all meshes; indices made absolute (no base-vertex draws
        // needed).
        std::vector<uint32_t> meshFirstIndex(s.meshes.size(), 0);
        size_t nv = 0, ni = 0;
        for (const rlios::io::Mesh& m : s.meshes) {
            nv += m.vertices.size();
            ni += m.indices.size();
        }
        vertices = nv ? [device newBufferWithLength:nv * sizeof(rlios::io::Vertex)
                                            options:MTLResourceStorageModeShared]
                      : nil;
        indices =
            ni ? [device newBufferWithLength:ni * 4 options:MTLResourceStorageModeShared] : nil;
        size_t vo = 0, ixo = 0;
        for (size_t k = 0; k < s.meshes.size(); ++k) {
            const rlios::io::Mesh& m = s.meshes[k];
            meshFirstIndex[k] = uint32_t(ixo);
            if (m.vertices.empty())
                continue;
            std::memcpy(static_cast<rlios::io::Vertex*>(vertices.contents) + vo, m.vertices.data(),
                        m.vertices.size() * sizeof(rlios::io::Vertex));
            uint32_t* dst = static_cast<uint32_t*>(indices.contents) + ixo;
            for (size_t i = 0; i < m.indices.size(); ++i)
                dst[i] = m.indices[i] + uint32_t(vo);
            vo += m.vertices.size();
            ixo += m.indices.size();
        }

        std::vector<InstanceGPU> inst(s.instances.size());
        opaque.clear();
        blended.clear();
        info.triangles = 0;
        for (size_t i = 0; i < s.instances.size(); ++i) {
            const rlios::io::Instance& in = s.instances[i];
            InstanceGPU& g = inst[i];
            auto row = [](const float* v, float w) {
                return simd_make_float4(v[0], v[1], v[2], w);
            };
            g.r0 = row(in.rows[0], 0.0f);
            g.r1 = row(in.rows[1], 0.0f);
            g.r2 = row(in.rows[2], 0.0f);
            g.r3 = row(in.rows[3], 1.0f);
            g.n0 = row(in.normalRows[0], 0.0f);
            g.n1 = row(in.normalRows[1], 0.0f);
            g.n2 = row(in.normalRows[2], 0.0f);
            const rlios::io::Mesh& m = s.meshes[in.mesh];
            const simd_float3 c = {(m.boundsMin[0] + m.boundsMax[0]) * 0.5f,
                                   (m.boundsMin[1] + m.boundsMax[1]) * 0.5f,
                                   (m.boundsMin[2] + m.boundsMax[2]) * 0.5f};
            const simd_float3 wc = c.x * g.r0.xyz + c.y * g.r1.xyz + c.z * g.r2.xyz + g.r3.xyz;
            for (size_t sec = 0; sec < m.sections.size(); ++sec) {
                const int32_t mi = s.slots[in.firstSlot + sec];
                if (mi < 0)
                    continue;
                const rlios::io::Material& mat = s.materials[size_t(mi)];
                if (mat.flags & (rlios::io::kMatSkip | rlios::io::kMatPlaceholder))
                    continue;
                if (mat.blend == rlios::io::Blend::Modulate)
                    continue; // needs a dedicated blend state; not used by the Stadium's visible
                              // meshes
                DrawItem di{uint32_t(i), uint32_t(mi),
                            meshFirstIndex[in.mesh] + m.sections[sec].firstIndex,
                            m.sections[sec].indexCount, wc};
                if (di.indexCount == 0)
                    continue;
                info.triangles += di.indexCount / 3;
                if (mat.blend == rlios::io::Blend::Translucent ||
                    mat.blend == rlios::io::Blend::Additive)
                    blended.push_back(di);
                else
                    opaque.push_back(di);
            }
        }
        // Opaque: group by material to cut state changes (stable, so deterministic).
        std::stable_sort(opaque.begin(), opaque.end(), [](const DrawItem& a, const DrawItem& b) {
            return a.material < b.material;
        });
        instances.swap(inst);
        info.instances = uint32_t(s.instances.size());
        info.draws = uint32_t(opaque.size() + blended.size());
        blendedOrder.resize(blended.size());
    }

    id<MTLTexture> depthFor(uint32_t w, uint32_t h, bool cache) {
        if (cache && depth && depth.width == w && depth.height == h)
            return depth;
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        if (@available(iOS 10.0, macOS 11.0, *)) {
            if (memorylessDepth)
                td.storageMode = MTLStorageModeMemoryless; // tile memory only (CLAUDE.md section 6)
        }
        id<MTLTexture> t = [device newTextureWithDescriptor:td];
        if (cache)
            depth = t;
        return t;
    }

    void camera(const RliosRenderState* rs, float aspect, simd_float3& eye,
                simd_float4x4& vp) const {
        if (shot >= 0 && uint32_t(shot) < kShotCount) {
            const Shot& s = kShots[shot];
            eye = s.eye;
            vp = viewProjection(s.eye, s.target, s.hfovDeg, aspect);
            return;
        }
        // Slow orbit around the field (one turn per 60 s of sim time).
        const double t = rs ? double(rs->tick % 7200) / 7200.0 : 0.0;
        const float a = float(t * 2.0 * M_PI) - 2.3f;
        eye = simd_make_float3(9500.0f * std::cos(a), 9500.0f * std::sin(a), 3800.0f);
        vp = viewProjection(eye, simd_make_float3(0.0f, 0.0f, 0.0f), 80.0f, aspect);
    }

    // Encodes one frame into `target`; returns the command buffer (not yet committed).
    id<MTLCommandBuffer> encode(id<MTLTexture> target, id<MTLTexture> depthTex, uint32_t w,
                                uint32_t h, const RliosRenderState* rs) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(kClearR, kClearG, kClearB, 1.0);
        pass.depthAttachment.texture = depthTex;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 0.0; // reversed Z

        SceneUniforms u{};
        simd_float3 eye;
        camera(rs, h ? float(w) / float(h) : 1.0f, eye, u.viewProj);
        u.cameraPos = simd_make_float4(eye, 1.0f);
        u.sunDir = simd_make_float4(simd_normalize(simd_make_float3(0.3f, 0.2f, 0.93f)), 0.0f);
        u.sunColor = simd_make_float4(0.75f, 0.73f, 0.68f, 1.0f);
        u.ambient = simd_make_float4(0.42f, 0.44f, 0.48f, 1.0f);
        // Fog colour = sky (clear colour, linear); density ~ 50% at 40000 uu.
        u.fogColor =
            simd_make_float4(srgbToLinear(51), srgbToLinear(102), srgbToLinear(204), 1.7e-5f);

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        if (vertices && indices && !instances.empty()) {
            [enc setCullMode:MTLCullModeNone]; // mirrored instances and two-sided materials; cheap
                                               // enough here
            [enc setVertexBuffer:vertices offset:0 atIndex:0];
            [enc setVertexBytes:&u length:sizeof(u) atIndex:2];
            [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
            [enc setFragmentSamplerState:sampler atIndex:0];
            [enc setRenderPipelineState:pipeOpaque];
            [enc setDepthStencilState:depthWrite];
            uint32_t lastMat = UINT32_MAX;
            for (const DrawItem& d : opaque)
                drawItem(enc, d, lastMat);
            if (!blended.empty()) {
                // Back to front by bounds centre.
                for (uint32_t i = 0; i < blendedOrder.size(); ++i)
                    blendedOrder[i] = i;
                std::vector<float> dist(blended.size());
                for (size_t i = 0; i < blended.size(); ++i)
                    dist[i] = simd_length_squared(blended[i].center - eye);
                std::stable_sort(blendedOrder.begin(), blendedOrder.end(),
                                 [&dist](uint32_t a, uint32_t b) { return dist[a] > dist[b]; });
                [enc setDepthStencilState:depthRead];
                id<MTLRenderPipelineState> cur = nil;
                lastMat = UINT32_MAX;
                for (uint32_t i : blendedOrder) {
                    const DrawItem& d = blended[i];
                    id<MTLRenderPipelineState> want =
                        materials[d.material].mode == 3 ? pipeAdditive : pipeTranslucent;
                    if (want != cur) {
                        [enc setRenderPipelineState:want];
                        cur = want;
                    }
                    drawItem(enc, d, lastMat);
                }
            }
        }
        [enc endEncoding];
        return cb;
    }

    void drawItem(id<MTLRenderCommandEncoder> enc, const DrawItem& d, uint32_t& lastMat) {
        if (d.material != lastMat) {
            const int32_t ti = materialTexture[d.material];
            [enc setFragmentTexture:(ti >= 0 ? textures[size_t(ti)] : white) atIndex:0];
            [enc setFragmentBytes:&materials[d.material] length:sizeof(MaterialGPU) atIndex:0];
            lastMat = d.material;
        }
        [enc setVertexBytes:&instances[d.instance] length:sizeof(InstanceGPU) atIndex:1];
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:d.indexCount
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:indices
                 indexBufferOffset:size_t(d.firstIndex) * 4];
    }

    void hookGpuTime(id<MTLCommandBuffer> cb) {
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
          const double s = done.GPUEndTime - done.GPUStartTime;
          if (s > 0.0)
              gpuMs.store(s * 1000.0);
        }];
    }
};

extern "C" {

RliosRenderer* rlios_renderer_create(void) {
    @autoreleasepool {
        RliosRenderer* r = new RliosRenderer();
        if (!r->init()) {
            delete r;
            return nullptr;
        }
        return r;
    }
}

void rlios_renderer_destroy(RliosRenderer* r) {
    delete r;
}

int rlios_renderer_load_scene(RliosRenderer* r, const char* packDir) {
    if (!r)
        return 0;
    @autoreleasepool {
        const double t0 = nowMs();
        const rlios::io::Scene s = rlios::io::loadSceneOrPlaceholder(packDir ? packDir : "");
        const double t1 = nowMs();
        r->upload(s);
        NSLog(@"RLiOS: scene %s: %s (load %.0f ms, upload %.0f ms, %u/%u textures on GPU, astc %u)",
              s.name.c_str(), s.status.c_str(), t1 - t0, nowMs() - t1, r->info.textures_gpu,
              r->info.textures, r->info.astc_supported);
        return s.placeholder ? 0 : 1;
    }
}

void rlios_renderer_scene_info(const RliosRenderer* r, RliosSceneInfo* out) {
    if (!out)
        return;
    *out = r ? r->info : RliosSceneInfo{};
}

uint32_t rlios_renderer_shot_count(void) {
    return kShotCount;
}

const char* rlios_renderer_shot_name(uint32_t shot) {
    return shot < kShotCount ? kShots[shot].name : "orbit";
}

void rlios_renderer_set_shot(RliosRenderer* r, int32_t shot) {
    if (r)
        r->shot = (shot >= 0 && uint32_t(shot) < kShotCount) ? shot : -1;
}

int rlios_renderer_draw_layer(RliosRenderer* r, void* layerPtr, uint32_t w, uint32_t h,
                              const RliosRenderState* rs, RliosFrameStats* stats) {
    if (!r || !layerPtr)
        return 0;
    @autoreleasepool {
        const double t0 = nowMs();
        CAMetalLayer* layer = (__bridge CAMetalLayer*)layerPtr;
        if (layer.device != r->device)
            layer.device = r->device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        const CGSize want = CGSizeMake(w, h);
        if (!CGSizeEqualToSize(layer.drawableSize, want))
            layer.drawableSize = want;
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable)
            return 0;
        id<MTLCommandBuffer> cb = r->encode(drawable.texture, r->depthFor(w, h, true), w, h, rs);
        r->hookGpuTime(cb);
        [cb presentDrawable:drawable];
        [cb commit];
        const uint64_t n = ++r->frames;
        if (stats) {
            stats->cpu_ms = nowMs() - t0;
            stats->gpu_ms = r->gpuMs.load();
            stats->frames = n;
        }
        return 1;
    }
}

int rlios_renderer_capture(RliosRenderer* r, const RliosRenderState* rs, uint32_t w, uint32_t h,
                           const char* pngPath, RliosFrameStats* stats) {
    if (!r || w == 0 || h == 0)
        return 0;
    @autoreleasepool {
        const double t0 = nowMs();
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [r->device newTextureWithDescriptor:td];
        id<MTLTexture> dtex = r->depthFor(w, h, false);
        if (!tex || !dtex)
            return 0;
        id<MTLCommandBuffer> cb = r->encode(tex, dtex, w, h, rs);
        r->hookGpuTime(cb);
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            NSLog(@"RLiOS: capture command buffer failed: %@", cb.error);
            return 0;
        }
        const uint64_t n = ++r->frames;
        if (stats) {
            stats->cpu_ms = nowMs() - t0;
            stats->gpu_ms = cb.GPUEndTime > cb.GPUStartTime
                                ? (cb.GPUEndTime - cb.GPUStartTime) * 1000.0
                                : r->gpuMs.load();
            stats->frames = n;
        }
        if (!pngPath)
            return 1;

        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        [tex getBytes:px.data()
            bytesPerRow:static_cast<NSUInteger>(w) * 4
             fromRegion:MTLRegionMake2D(0, 0, w, h)
            mipmapLevel:0];
        // BGRA8 little-endian host order == kCGBitmapByteOrder32Little | premultiplied-first.
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx =
            CGBitmapContextCreate(px.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
                                  kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
        bool ok = false;
        if (img) {
            NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:pngPath]];
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
                (__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
            if (dst) {
                CGImageDestinationAddImage(dst, img, nullptr);
                ok = CGImageDestinationFinalize(dst);
                CFRelease(dst);
            }
            CGImageRelease(img);
        }
        if (ctx)
            CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        return ok ? 1 : 0;
    }
}

} // extern "C"
