#include "hardware_accel_vulkan_helper.h"
#include "include/decodersdk/vulkan_wrapper_define.h"

extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace vulkan_helper {
bool vulkanDeviceContextIsValid(void *userContext)
{
    VulkanDeviceContext *context = static_cast<VulkanDeviceContext *>(userContext);
    return context != nullptr && context->inst != VK_NULL_HANDLE &&
           context->phys_dev != VK_NULL_HANDLE && context->act_dev != VK_NULL_HANDLE;
}

void transToAVVulkanDeviceContext(AVHWDeviceContext *deviceContext, void *userContext)
{
    AVVulkanDeviceContext *vulkanContext = (AVVulkanDeviceContext *)deviceContext->hwctx;
    VulkanDeviceContext *context = static_cast<VulkanDeviceContext *>(userContext);

    vulkanContext->get_proc_addr = context->get_proc_addr;
    vulkanContext->inst = context->inst;
    vulkanContext->phys_dev = context->phys_dev;
    vulkanContext->act_dev = context->act_dev;
    vulkanContext->device_features = context->device_features;

    // 设置扩展信息
    vulkanContext->enabled_inst_extensions = context->enabled_inst_extensions;
    vulkanContext->nb_enabled_inst_extensions = context->nb_enabled_inst_extensions;
    vulkanContext->enabled_dev_extensions = context->enabled_dev_extensions;
    vulkanContext->nb_enabled_dev_extensions = context->nb_enabled_dev_extensions;

    // 队列相关
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(59, 34, 100)
    vulkanContext->queue_family_index = -1;
    vulkanContext->nb_graphics_queues = 0;
    vulkanContext->queue_family_tx_index = -1;
    vulkanContext->nb_tx_queues = 0;
    vulkanContext->queue_family_comp_index = -1;
    vulkanContext->nb_comp_queues = 0;
    vulkanContext->queue_family_encode_index = -1;
    vulkanContext->nb_encode_queues = 0;
    vulkanContext->queue_family_decode_index = -1;
    vulkanContext->nb_decode_queues = 0;
#endif

    for (int i = 0; i < context->nb_qf; ++i) {
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 34, 100)
        vulkanContext->qf[i].idx = context->qf[i].idx;
        vulkanContext->qf[i].num = context->qf[i].num;
        vulkanContext->qf[i].flags = context->qf[i].flags;
        vulkanContext->qf[i].video_caps = context->qf[i].video_caps;
#else
        if (context->qf[i].flags == VK_QUEUE_GRAPHICS_BIT) {
            vulkanContext->queue_family_index = context->qf[i].idx;
            vulkanContext->nb_graphics_queues = context->qf[i].num;
        } else if (context->qf[i].flags == VK_QUEUE_TRANSFER_BIT) {
            vulkanContext->queue_family_tx_index = context->qf[i].idx;
            vulkanContext->nb_tx_queues = context->qf[i].num;
        } else if (context->qf[i].flags == VK_QUEUE_COMPUTE_BIT) {
            vulkanContext->queue_family_comp_index = context->qf[i].idx;
            vulkanContext->nb_comp_queues = context->qf[i].num;
        } else if (context->qf[i].flags == VK_QUEUE_VIDEO_ENCODE_BIT_KHR) {
            vulkanContext->queue_family_encode_index = context->qf[i].idx;
            vulkanContext->nb_encode_queues = context->qf[i].num;
        } else if (context->qf[i].flags == VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            vulkanContext->queue_family_decode_index = context->qf[i].idx;
            vulkanContext->nb_decode_queues = context->qf[i].num;
        }
#endif
    }
    vulkanContext->nb_qf = context->nb_qf;

    //// debug info
    //for (int i = 0; i < vulkanContext->nb_enabled_inst_extensions; ++i) {
    //    printf("enabled_inst_extensions[%d]: %s\n", i, vulkanContext->enabled_inst_extensions[i]);
    //}
    //for (int i = 0; i < context->nb_enabled_dev_extensions; ++i) {
    //    printf("enabled_dev_extensions[%d]: %s\n", i, context->enabled_dev_extensions[i]);
    //}
}

AVPixelFormat getPixelFormat(AVCodecContext *codecCtx, const enum AVPixelFormat *pix_fmts)
{
    if (avcodec_get_hw_frames_parameters(codecCtx, codecCtx->hw_device_ctx, AV_PIX_FMT_VULKAN,
                                         &codecCtx->hw_frames_ctx) < 0) {
        return AV_PIX_FMT_NONE;
    }

    auto *frames = reinterpret_cast<AVHWFramesContext *>(codecCtx->hw_frames_ctx->data);
    auto *vk = static_cast<AVVulkanFramesContext *>(frames->hwctx);
    // We take views of individual planes if we don't get a clean YCbCr sampler, need
    // this.
    vk->img_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;

    if (av_hwframe_ctx_init(codecCtx->hw_frames_ctx) < 0) {
        av_buffer_unref(&codecCtx->hw_frames_ctx);
        return AV_PIX_FMT_NONE;
    }

    return AV_PIX_FMT_VULKAN;
}

} // namespace vulkan_helper

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END