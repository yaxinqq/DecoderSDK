#ifndef VAAPI_UTILS_H
#define VAAPI_UTILS_H

extern "C" {
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include "base/base_define.h"
#include "vaapi_compat.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace va_wrapper {
/**
 * @brief 检查VA状态是否为VA_STATUS_SUCCESS，如果不是则输出错误信息
 *
 * @param va_status VA状态
 * @param msg 上下文信息
 * @return true 如果状态为VA_STATUS_SUCCESS，否则返回false
 */
extern bool va_check_status(VAStatus va_status, const char *msg);

/**
 * @brief 销毁VA配置对象
 *
 * 安全地销毁VAConfigID，如果配置ID有效则调用vaDestroyConfig，
 * 销毁后将配置ID设置为VA_INVALID_ID。
 *
 * @param dpy VADisplay句柄
 * @param cfg_ptr 指向VAConfigID的指针，销毁后会被设置为VA_INVALID_ID
 */
extern void va_destroy_config(VADisplay dpy, VAConfigID *cfg_ptr);

/**
 * @brief 销毁VA上下文对象
 *
 * 安全地销毁VAContextID，如果上下文ID有效则调用vaDestroyContext，
 * 销毁后将上下文ID设置为VA_INVALID_ID。
 *
 * @param dpy VADisplay句柄
 * @param ctx_ptr 指向VAContextID的指针，销毁后会被设置为VA_INVALID_ID
 */
extern void va_destroy_context(VADisplay dpy, VAContextID *ctx_ptr);

/**
 * @brief 销毁VA表面对象
 *
 * 安全地销毁VASurfaceID，如果表面ID有效则调用vaDestroySurfaces，
 * 销毁后将表面ID设置为VA_INVALID_ID。
 *
 * @param dpy VADisplay句柄
 * @param surf_ptr 指向VASurfaceID的指针，销毁后会被设置为VA_INVALID_ID
 */
extern void va_destroy_surface(VADisplay dpy, VASurfaceID *surf_ptr);

/**
 * @brief 销毁VA缓冲区对象
 *
 * 安全地销毁VABufferID，如果缓冲区ID有效则调用vaDestroyBuffer，
 * 销毁后将缓冲区ID设置为VA_INVALID_ID。
 *
 * @param dpy VADisplay句柄
 * @param buf_ptr 指向VABufferID的指针，销毁后会被设置为VA_INVALID_ID
 */
extern void va_destroy_buffer(VADisplay dpy, VABufferID *buf_ptr);

/**
 * @brief 销毁VA缓冲区数组
 *
 * 批量销毁VABufferID数组中的所有缓冲区，销毁后将数组长度设置为0。
 *
 * @param dpy VADisplay句柄
 * @param buf VABufferID数组指针
 * @param len_ptr 指向数组长度的指针，销毁后会被设置为0
 */
extern void va_destroy_buffers(VADisplay dpy, VABufferID *buf, uint32_t *len_ptr);

/**
 * @brief 创建并映射VA缓冲区
 *
 * 创建指定类型和大小的VA缓冲区，可选择性地映射缓冲区到内存。
 * 如果映射失败，会自动清理已创建的缓冲区。
 *
 * @param dpy VADisplay句柄
 * @param ctx VAContextID上下文
 * @param type 缓冲区类型
 * @param size 缓冲区大小
 * @param data 初始数据指针，可为NULL
 * @param buf_id_ptr 输出参数，返回创建的缓冲区ID
 * @param mapped_data_ptr 输出参数，如果非NULL则返回映射的内存地址
 * @return true 创建成功，false 创建失败
 */
extern bool va_create_buffer(VADisplay dpy, VAContextID ctx, int type, size_t size,
                             const void *data, VABufferID *buf_id_ptr, void **mapped_data_ptr);

/**
 * @brief 映射指定的VA缓冲区到内存
 *
 * 将VA缓冲区映射到CPU可访问的内存地址。
 *
 * @param dpy VADisplay句柄
 * @param buf_id 要映射的缓冲区ID
 * @return void* 映射的内存地址，失败时返回NULL
 */
extern void *va_map_buffer(VADisplay dpy, VABufferID buf_id);

/**
 * @brief 取消映射VA缓冲区
 *
 * 取消VA缓冲区的内存映射，并可选择性地将数据指针设置为NULL。
 *
 * @param dpy VADisplay句柄
 * @param buf_id 要取消映射的缓冲区ID
 * @param buf_ptr 可选的数据指针，取消映射后会被设置为NULL
 */
extern void va_unmap_buffer(VADisplay dpy, VABufferID buf_id, void **buf_ptr);

/**
 * @brief 使用安全的默认值初始化VAImage结构
 *
 * 将VAImage结构的成员设置为安全的默认值，防止使用未初始化的数据。
 *
 * @param image 指向VAImage结构的指针
 */
extern void va_image_init_defaults(VAImage *image);

/**
 * @brief 创建DRM后端的VADisplay
 *
 * 该函数会尝试打开DRM设备（优先使用render节点，如/dev/dri/renderD128），
 * 然后创建VADisplay并进行初始化。支持自动回退到card节点。
 *
 * @param fd 输出参数，返回打开的DRM设备文件描述符
 * @return VADisplay 成功时返回有效的VADisplay，失败时返回空值
 */
extern VADisplay createDrmVADisplay(int &fd);

/**
 * @brief 销毁DRM后端的VADisplay并关闭相关资源
 *
 * 安全地终止VADisplay并关闭DRM设备文件描述符，清理所有相关资源。
 *
 * @param vaDisplay VADisplay引用，销毁后会被设置为nullptr
 * @param fd DRM设备文件描述符引用，关闭后会被设置为-1
 */
extern void destoryDrmVADisplay(VADisplay &vaDisplay, int &fd);

/**
 * @brief 导出VA Surface的DMA-BUF句柄
 *
 * 该函数将VA Surface导出为DRM PRIME格式的表面描述符，
 * 用于与其他图形API（如OpenGL/EGL）进行零拷贝互操作。
 * 导出的描述符包含DMA-BUF文件描述符和相关的格式信息。
 *
 * @param vaDisplay VADisplay句柄
 * @param vaSurfaceID 要导出的VA Surface ID
 * @return VADRMPRIMESurfaceDescriptor 包含DMA-BUF信息的描述符，失败时返回空描述符
 */
extern VADRMPRIMESurfaceDescriptor exportVASurfaceHandle(VADisplay vaDisplay,
                                                         VASurfaceID vaSurfaceID);

/**
 * @brief 同步VA Surface
 *
 * 等待指定的VA Surface上的所有操作完成，确保数据一致性。
 * 通常在读取Surface数据前或进行零拷贝操作前调用。
 *
 * @param vaDisplay VADisplay句柄
 * @param vaSurfaceID 要同步的VA Surface ID
 */
extern void syncVASurface(VADisplay vaDisplay, VASurfaceID vaSurfaceID);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // VAAPI_UTILS_H
