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

/* Match: a bot-vs-bot soccar match (rules in core/game; placeholder physics until the real
 * car/ball sim is wired in). Manual stepping only; deterministic for a given config. */
typedef struct RliosMatch RliosMatch;

typedef struct {
    uint32_t blue_cars;   /* 1..4 */
    uint32_t orange_cars; /* 1..4 */
    uint64_t seed;
} RliosMatchConfig;

typedef struct {
    uint64_t tick;
    uint32_t phase; /* 0 countdown, 1 active, 2 post-goal, 3 ended */
    uint32_t overtime;
    int32_t score_blue, score_orange;
    int32_t seconds_remaining; /* regulation clock as shown */
    int32_t winner;            /* -1 until ended, else 0 blue / 1 orange */
    uint32_t num_cars;
    uint32_t total_demos;
    uint32_t total_goals;
    float ball_x, ball_y, ball_z;
    uint64_t state_hash;
} RliosMatchInfo;

typedef struct {
    float x, y, z;
    float boost; /* 0..1 */
    uint32_t team;
    uint32_t demolished;
} RliosCarInfo;

RliosMatch* rlios_match_create(const RliosMatchConfig* cfg);
void rlios_match_destroy(RliosMatch* m);
/* Runs up to `ticks` 120 Hz ticks with bot inputs; stops early when the match ends.
 * Returns 1 if the match has ended. */
int rlios_match_step(RliosMatch* m, uint32_t ticks);
void rlios_match_get_info(const RliosMatch* m, RliosMatchInfo* out);
/* Returns 1 if `index` is a valid car. */
int rlios_match_get_car(const RliosMatch* m, uint32_t index, RliosCarInfo* out);

/* Render (Metal; returns NULL / 0 on platforms without Metal) */
RliosRenderer* rlios_renderer_create(void);
void rlios_renderer_destroy(RliosRenderer* r);
/* Draw into a CAMetalLayer* (passed as void*). Returns 1 if a frame was presented. */
int rlios_renderer_draw_layer(RliosRenderer* r, void* ca_metal_layer, uint32_t w, uint32_t h,
                              const RliosRenderState* rs, RliosFrameStats* stats);
/* Render offscreen (w x h BGRA8) and optionally write a PNG (png_path may be NULL). */
int rlios_renderer_capture(RliosRenderer* r, const RliosRenderState* rs, uint32_t w, uint32_t h,
                           const char* png_path, RliosFrameStats* stats);

#ifdef __cplusplus
}
#endif

#endif
