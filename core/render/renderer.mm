// Metal renderer (Apple only). C++-style core with a C API (see api/rlios.h); Swift stays thin.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>

#include "api/rlios.h"

namespace {

// Distinctive clear color: bytes (51, 102, 204) in BGRA8Unorm.
constexpr double kClearR = 51.0 / 255.0;
constexpr double kClearG = 102.0 / 255.0;
constexpr double kClearB = 204.0 / 255.0;

struct FrameUniforms {
    float center[2];
    float aspect;
    float radius;
    float discColor[4];
};

double nowMs() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct RliosRenderer {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    std::atomic<double> gpuMs{0.0};
    std::atomic<uint64_t> frames{0};

    // Prebuilt at creation (CLAUDE.md section 6: no pipeline creation mid-match).
    bool init() {
        device = MTLCreateSystemDefaultDevice();
        if (!device)
            return false;
        queue = [device newCommandQueue];
        id<MTLLibrary> lib = [device newDefaultLibrary];
        if (!lib)
            return false;
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"rlios_vs"];
        d.fragmentFunction = [lib newFunctionWithName:@"rlios_fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        NSError* err = nil;
        pipeline = [device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pipeline) {
            NSLog(@"RLiOS: pipeline error: %@", err);
            return false;
        }
        return true;
    }

    // Encodes one frame into `target`; returns the command buffer (not yet committed).
    id<MTLCommandBuffer> encode(id<MTLTexture> target, uint32_t w, uint32_t h,
                                const RliosRenderState* rs) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(kClearR, kClearG, kClearB, 1.0);

        FrameUniforms u{};
        u.center[0] = rs ? rs->x : 0.0f;
        u.center[1] = rs ? rs->y : 0.0f;
        u.aspect = h ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
        u.radius = 0.06f;
        const float pulse =
            rs ? 0.5f + 0.5f * std::sin(static_cast<float>(rs->tick % 240) * (6.2831853f / 240.0f))
               : 0.5f;
        u.discColor[0] = 1.0f;
        u.discColor[1] = 0.55f + 0.4f * pulse;
        u.discColor[2] = 0.1f;
        u.discColor[3] = 1.0f;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:pipeline];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        return cb;
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
        id<MTLCommandBuffer> cb = r->encode(drawable.texture, w, h, rs);
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
        if (!tex)
            return 0;
        id<MTLCommandBuffer> cb = r->encode(tex, w, h, rs);
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
            stats->gpu_ms = r->gpuMs.load();
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
