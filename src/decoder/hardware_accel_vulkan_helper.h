#ifndef DECODER_SDK_INTERNAL_HARDWARE_ACCEL_VULKAN_CONTEXT_H
#define DECODER_SDK_INTERNAL_HARDWARE_ACCEL_VULKAN_CONTEXT_H
extern "C" {
#include "libavcodec/avcodec.h"
}

#include "base/base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace vulkan_helper {
bool vulkanDeviceContextIsValid(void *userContext);
void transToAVVulkanDeviceContext(AVHWDeviceContext *deviceContext, void *userContext);
AVPixelFormat getPixelFormat(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
} // namespace vulkan_helper

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_HARDWARE_ACCEL_VULKAN_CONTEXT_H