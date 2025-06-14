#include "common_utils.h"

#include <algorithm>
#include <cmath>
#include <thread>

extern "C" {
#include <libavutil/error.h>
}

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

namespace utils {
bool equal(double a, double b, double epsilon)
{
    return std::fabs(a - b) < epsilon;
}
bool equal(float a, float b, double epsilon)
{
    return std::fabs(a - b) < epsilon;
}

// a > b 返回true
bool greater(double a, double b, double epsilon)
{
    return a > b && (a - b) > epsilon;
}
bool greater(float a, float b, double epsilon)
{
    return a > b && (a - b) > epsilon;
}

// a >= b 返回true
bool greaterAndEqual(double a, double b, double epsilon)
{
    return greater(a, b, epsilon) || equal(a, b, epsilon);
}
bool greaterAndEqual(float a, float b, double epsilon)
{
    return greater(a, b, epsilon) || equal(a, b, epsilon);
}

void highPrecisionSleep(double ms, std::chrono::steady_clock::time_point startTime)
{
    std::this_thread::sleep_until(startTime +
                                  std::chrono::microseconds(static_cast<int>(ms * 1000)));
    // using namespace std;
    // using namespace std::chrono;

    // static double estimate = 5e-3;
    // static double mean = 5e-3;
    // static double m2 = 0;
    // static int64_t count = 1;

    // double seconds = ms / 1000;
    // while (seconds > estimate) {
    //     auto start = high_resolution_clock::now();
    //     this_thread::sleep_for(milliseconds(1));
    //     auto end = high_resolution_clock::now();

    //     double observed = (end - start).count() / 1e9;
    //     seconds -= observed;

    //     ++count;
    //     double delta = observed - mean;
    //     mean += delta / count;
    //     m2   += delta * (observed - mean);
    //     double stddev = sqrt(m2 / (count - 1));
    //     estimate = mean + stddev;
    // }

    // // spin lock
    // auto start = high_resolution_clock::now();
    // while ((high_resolution_clock::now() - start).count() / 1e9 < seconds);
}

bool isRealtime(const std::string &url)
{
    std::string u = url;
    std::transform(u.begin(), u.end(), u.begin(), ::tolower);

    static const char *realtimePrefixes[] = {"rtsp://", "rtmp://", "udp://",
                                             "tcp://",  "srt://",  "mms://"};

    for (const char *prefix : realtimePrefixes) {
        if (u.find(prefix) == 0)
            return true;
    }

    if ((u.find("http://") == 0 || u.find("https://") == 0) &&
        (u.find(".m3u8") != std::string::npos || u.find("/live/") != std::string::npos ||
         u.find("stream") != std::string::npos)) {
        return true;
    }

    return false;
}

std::string avErr2Str(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, errnum);
    return std::string(buf);
}

AVPixelFormat imageFormat2AVPixelFormat(ImageFormat format)
{
    switch (format) {
        case ImageFormat::kNV12:
            return AV_PIX_FMT_NV12;
        case ImageFormat::kNV21:
            return AV_PIX_FMT_NV21;
        case ImageFormat::kYUV420P:
            return AV_PIX_FMT_YUV420P;
        case ImageFormat::kYUV422P:
            return AV_PIX_FMT_YUV422P;
        case ImageFormat::kYUV444P:
            return AV_PIX_FMT_YUV444P;
        case ImageFormat::kRGB24:
            return AV_PIX_FMT_RGB24;
        case ImageFormat::kBGR24:
            return AV_PIX_FMT_BGR24;
        case ImageFormat::kRGBA:
            return AV_PIX_FMT_RGBA;
        case ImageFormat::kBGRA:
            return AV_PIX_FMT_BGRA;
        case ImageFormat::kDxva2:
            return AV_PIX_FMT_DXVA2_VLD;
        case ImageFormat::kD3d11va:
            return AV_PIX_FMT_D3D11;
        case ImageFormat::kCuda:
            return AV_PIX_FMT_CUDA;
        case ImageFormat::kVaapi:
            return AV_PIX_FMT_VAAPI;
        case ImageFormat::kVdpau:
            return AV_PIX_FMT_VDPAU;
        case ImageFormat::kQsv:
            return AV_PIX_FMT_QSV;
        case ImageFormat::kVideoToolBox:
            return AV_PIX_FMT_VIDEOTOOLBOX;
        default:
            break;
    }
    return AV_PIX_FMT_NONE;
}

ImageFormat avPixelFormat2ImageFormat(AVPixelFormat format)
{
    switch (format) {
        case AV_PIX_FMT_NV12:
            return ImageFormat::kNV12;
        case AV_PIX_FMT_NV21:
            return ImageFormat::kNV21;
        case AV_PIX_FMT_YUV420P:
            return ImageFormat::kYUV420P;
        case AV_PIX_FMT_YUV422P:
            return ImageFormat::kYUV422P;
        case AV_PIX_FMT_YUV444P:
            return ImageFormat::kYUV444P;
        case AV_PIX_FMT_RGB24:
            return ImageFormat::kRGB24;
        case AV_PIX_FMT_BGR24:
            return ImageFormat::kBGR24;
        case AV_PIX_FMT_RGBA:
            return ImageFormat::kRGBA;
        case AV_PIX_FMT_BGRA:
            return ImageFormat::kBGRA;
        case AV_PIX_FMT_DXVA2_VLD:
            return ImageFormat::kDxva2;
        case AV_PIX_FMT_D3D11:
            return ImageFormat::kD3d11va;
        case AV_PIX_FMT_CUDA:
            return ImageFormat::kCuda;
        case AV_PIX_FMT_VAAPI:
            return ImageFormat::kVaapi;
        case AV_PIX_FMT_VDPAU:
            return ImageFormat::kVdpau;
        case AV_PIX_FMT_QSV:
            return ImageFormat::kQsv;
        case AV_PIX_FMT_VIDEOTOOLBOX:
            return ImageFormat::kVideoToolBox;
        default:
            break;
    }
    return ImageFormat::kUnknown;
}
} // namespace utils

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END