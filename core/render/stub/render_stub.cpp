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
}
