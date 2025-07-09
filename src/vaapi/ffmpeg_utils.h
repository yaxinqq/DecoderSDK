#ifndef FFMPEG_UTILS_H
#define FFMPEG_UTILS_H

extern "C" {
#include <va/va.h>
}

#include "ffmpeg_compat.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace va_wrapper {
/**
 * @brief 获取FFmpeg错误码的字符串表示
 *
 * 将FFmpeg错误码转换为可读的错误信息字符串。
 * 如果av_strerror失败，则返回通用的错误格式。
 *
 * @param errnum FFmpeg错误码
 * @param errbuf 错误信息缓冲区，大小应为BUFSIZ
 * @return const char* 错误信息字符串
 */
const char *ffmpeg_strerror(int errnum, char errbuf[BUFSIZ]);

/**
 * @brief 将FFmpeg编解码器和配置文件转换为VAAPI配置文件
 *
 * 根据FFmpeg的编解码器ID和配置文件，查找对应的VAAPI配置文件。
 * 支持MPEG2、MPEG4、H.264、VC1、HEVC、VP8、VP9等编解码器。
 *
 * @param ff_codec FFmpeg编解码器ID
 * @param ff_profile FFmpeg配置文件
 * @param profile_ptr 输出参数，返回对应的VAAPI配置文件
 * @return true 转换成功，false 不支持的编解码器或配置文件
 */
bool ffmpeg_to_vaapi_profile(enum AVCodecID ff_codec, int ff_profile, VAProfile *profile_ptr);

/**
 * @brief 将FFmpeg像素格式转换为VAAPI格式
 *
 * 将FFmpeg的像素格式转换为VAAPI的fourcc和chroma格式。
 * 支持常见的YUV和RGB格式，如NV12、YUV420P、YUYV422等。
 *
 * @param pix_fmt FFmpeg像素格式
 * @param fourcc_ptr 输出参数，返回VAAPI的fourcc格式
 * @param chroma_ptr 输出参数，返回VAAPI的chroma格式
 * @return true 转换成功，false 不支持的像素格式
 */
bool ffmpeg_to_vaapi_pix_fmt(enum AVPixelFormat pix_fmt, uint32_t *fourcc_ptr,
                             uint32_t *chroma_ptr);

/**
 * @brief 将VAAPI fourcc格式转换为FFmpeg像素格式
 *
 * 将VAAPI的fourcc格式转换回FFmpeg的像素格式。
 * 这是ffmpeg_to_vaapi_pix_fmt的逆向操作。
 *
 * @param fourcc VAAPI的fourcc格式
 * @param pix_fmt_ptr 输出参数，返回对应的FFmpeg像素格式
 * @return true 转换成功，false 不支持的fourcc格式
 */
bool vaapi_to_ffmpeg_pix_fmt(uint32_t fourcc, enum AVPixelFormat *pix_fmt_ptr);

/**
 * @brief 将VAAPI状态码转换为FFmpeg错误码
 *
 * 将VAAPI的VAStatus状态码映射为对应的FFmpeg错误码。
 * 用于统一错误处理和错误传播。
 *
 * @param va_status VAAPI状态码
 * @return int 对应的FFmpeg错误码（负值）
 */
int vaapi_to_ffmpeg_error(VAStatus va_status);

/**
 * @brief 将YUV数据保存到文件
 *
 * 将VAImage中的YUV数据按照NV12格式写入文件。
 * 先写入Y平面，再写入UV平面（交错格式）。
 *
 * @param file 目标文件指针
 * @param yuv_data YUV数据缓冲区
 * @param width 图像宽度
 * @param height 图像高度
 * @param va_image VAImage结构，包含偏移和步长信息
 */
void save_yuv_to_file(FILE *file, unsigned char *yuv_data, int width, int height,
                      VAImage *va_image);

/**
 * @brief 从VA Surface获取图像数据
 *
 * 创建VAImage并从指定的VA Surface获取图像数据。
 * 主要用于调试和测试目的，获取解码后的图像内容。
 *
 * @param va_dpy VADisplay句柄
 * @param surface VA Surface ID
 * @param width 图像宽度
 * @param height 图像高度
 */
void get_surface_image(VADisplay va_dpy, VASurfaceID surface, int width, int height);

/**
 * @brief 将VA Surface保存为文件
 *
 * 将指定的VA Surface内容保存为YUV文件。
 * 创建VAImage，获取Surface数据，映射到CPU内存，然后写入文件。
 * 适用于调试解码结果和验证图像质量。
 *
 * @param va_dpy VADisplay句柄
 * @param surface VA Surface ID
 * @param width 图像宽度
 * @param height 图像高度
 * @param filename 输出文件名
 * @return int 0表示成功，-1表示失败
 */
int save_surface_to_file(VADisplay va_dpy, VASurfaceID surface, int width, int height,
                         const char *filename);
}

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif /* FFMPEG_UTILS_H */