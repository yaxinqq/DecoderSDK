#ifndef DECODER_SDK_VAAPI_UTILS_H
#define DECODER_SDK_VAAPI_UTILS_H

#ifdef VAAPI_AVAILABLE
extern "C" {
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include <string>

#include "sdk_global.h"

namespace decoder_sdk {
/**
 * @brief 创建一个DRM VADisplay
 *
 * @param fd 输出的文件描述符
 * @param deviceIndex DRM设备索引
 * @return 返回的VADisplay对象
 */
VADisplay DECODER_SDK_API createDrmVADisplay(int &fd, int deviceIndex);

/**
 * @brief 销毁一个DRM VADisplay
 *
 * @param vaDisplay 要销毁的VADisplay对象，会被修改为nullptr
 * @param fd 输出的文件描述符，销毁后会被设置为-1
 */
void DECODER_SDK_API destoryDrmVADisplay(VADisplay &vaDisplay, int &fd);

/**
 * @brief 导出VA Surface句柄
 *
 * @param vaDisplay VADisplay对象
 * @param vaSurfaceID VASurfaceID
 * @return 返回的VASurfaceDescriptor
 */
VADRMPRIMESurfaceDescriptor DECODER_SDK_API exportVASurfaceHandle(VADisplay vaDisplay,
                                                                  VASurfaceID vaSurfaceID);

/**
 * @brief 同步VA Surface
 *
 * @param vaDisplay VADisplay对象
 * @param vaSurfaceID VASurfaceID
 */
void DECODER_SDK_API syncVASurface(VADisplay vaDisplay, VASurfaceID vaSurfaceID);
} // namespace decoder_sdk
#endif

#endif // DECODER_SDK_VAAPI_UTILS_H