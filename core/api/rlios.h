/* Small C API between the Swift shell and the C++ core. */
#ifndef RLIOS_H
#define RLIOS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RliosSim RliosSim;
typedef struct RliosRenderer RliosRenderer;

typedef struct {
    uint32_t manual_stepping;    /* 1: no sim thread, caller drives rlios_step() */
    uint32_t max_catch_up_ticks; /* 0: default (16) */
} RliosConfig;

typedef struct {
    uint64_t tick;
    double alpha; /* interpolation factor between the two latest snapshots */
    float x, y;
    float vx, vy;
    uint64_t state_hash;
} RliosRenderState;

typedef struct {
    uint64_t tick;
    uint64_t state_hash;
    uint64_t dropped_ticks;
    double tick_rate_hz;
    uint32_t running;
} RliosDiagnostics;

typedef struct {
    double cpu_ms; /* CPU time to encode+submit the last frame */
    double gpu_ms; /* GPU time of the last completed frame (0 if unavailable) */
    uint64_t frames;
} RliosFrameStats;

typedef struct {
    uint32_t real_pack;    /* 1: scene came from the asset pack; 0: built-in placeholder arena */
    uint32_t instances;    /* placed mesh instances */
    uint32_t draws;        /* draw calls per frame (instance sections that are drawn) */
    uint64_t triangles;    /* triangles per frame */
    uint32_t textures;     /* textures in the scene */
    uint32_t textures_gpu; /* textures uploaded to the GPU (others use their average colour) */
    uint32_t astc_supported;
    char status[256]; /* loader status line */
} RliosSceneInfo;

/* Monotonic clock in ns (the clock the sim thread uses). */
uint64_t rlios_now_ns(void);

/* Sim */
RliosSim* rlios_create(const RliosConfig* cfg);
void rlios_destroy(RliosSim* sim);
int rlios_start(RliosSim* sim);
void rlios_stop(RliosSim* sim);
void rlios_set_input(RliosSim* sim, float ax, float ay);
void rlios_step(RliosSim* sim, uint32_t ticks);
/* Consumer side: call from ONE thread per RliosSim. Returns 1 on success. */
int rlios_get_render_state(RliosSim* sim, uint64_t now_ns, RliosRenderState* out);
void rlios_get_diagnostics(const RliosSim* sim, RliosDiagnostics* out);

/* Render (Metal; returns NULL / 0 on platforms without Metal) */
RliosRenderer* rlios_renderer_create(void);
void rlios_renderer_destroy(RliosRenderer* r);
/* Draw into a CAMetalLayer* (passed as void*). Returns 1 if a frame was presented. */
int rlios_renderer_draw_layer(RliosRenderer* r, void* ca_metal_layer, uint32_t w, uint32_t h,
                              const RliosRenderState* rs, RliosFrameStats* stats);
/* Render offscreen (w x h BGRA8) and optionally write a PNG (png_path may be NULL). */
int rlios_renderer_capture(RliosRenderer* r, const RliosRenderState* rs, uint32_t w, uint32_t h,
                           const char* png_path, RliosFrameStats* stats);

/* Load the scene from an asset pack directory (manifest.txt inside). Falls back to the placeholder
   arena (also used until this is called). Returns 1 if the real pack was loaded. Not mid-match. */
int rlios_renderer_load_scene(RliosRenderer* r, const char* pack_dir);
void rlios_renderer_scene_info(const RliosRenderer* r, RliosSceneInfo* out);
/* Fixed camera shots for captures; -1 (default) = slow orbit driven by the sim tick. */
uint32_t rlios_renderer_shot_count(void);
const char* rlios_renderer_shot_name(uint32_t shot);
void rlios_renderer_set_shot(RliosRenderer* r, int32_t shot);

#ifdef __cplusplus
}
#endif

#endif
