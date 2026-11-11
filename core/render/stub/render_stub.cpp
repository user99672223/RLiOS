// Non-Apple platforms: no Metal renderer. The C API reports "unavailable".
#include "api/rlios.h"

extern "C" {
RliosRenderer* rlios_renderer_create(void) {
    return nullptr;
}
void rlios_renderer_destroy(RliosRenderer*) {}
int rlios_renderer_draw_layer(RliosRenderer*, void*, uint32_t, uint32_t, const RliosRenderState*,
                              RliosFrameStats*) {
    return 0;
}
int rlios_renderer_capture(RliosRenderer*, const RliosRenderState*, uint32_t, uint32_t, const char*,
                           RliosFrameStats*) {
    return 0;
}
int rlios_renderer_load_scene(RliosRenderer*, const char*) {
    return 0;
}
void rlios_renderer_scene_info(const RliosRenderer*, RliosSceneInfo* out) {
    if (out)
        *out = RliosSceneInfo{};
}
uint32_t rlios_renderer_shot_count(void) {
    return 0;
}
const char* rlios_renderer_shot_name(uint32_t) {
    return "";
}
void rlios_renderer_set_shot(RliosRenderer*, int32_t) {}
}
