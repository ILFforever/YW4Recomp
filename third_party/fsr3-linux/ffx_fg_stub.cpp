// The frame-interpolation swapchain (sdk/src/backends/vk/FrameInterpolationSwapchain) is not
// built on Linux. The backend stores this function's address in its interface table; the
// upscaler never calls it. Frame generation is a separate, later objective.
#include <FidelityFX/host/backends/vk/ffx_vk.h>

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const* config)
{
    (void)config;
    return FFX_ERROR_BACKEND_API_ERROR;
}
