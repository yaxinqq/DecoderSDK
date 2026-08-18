#include "InternalUtils.h"
#include "CommonUtils.h"

namespace utils {
    decoder_sdk::ImageFormat transImageFormatString(const QString &str)
    {
        const QString incaseStr = str.toUpper();

        if (incaseStr == QStringLiteral("YUV420P"))
            return decoder_sdk::ImageFormat::kYUV420P;
        else if (incaseStr == QStringLiteral("YUV422P"))
            return decoder_sdk::ImageFormat::kYUV422P;
        else if (incaseStr == QStringLiteral("YUV444P"))
            return decoder_sdk::ImageFormat::kYUV444P;
        else if (incaseStr == QStringLiteral("RGB24"))
            return decoder_sdk::ImageFormat::kRGB24;
        else if (incaseStr == QStringLiteral("BGR24"))
            return decoder_sdk::ImageFormat::kBGR24;
        else if (incaseStr == QStringLiteral("RGBA"))
            return decoder_sdk::ImageFormat::kRGBA;
        else if (incaseStr == QStringLiteral("BGRA"))
            return decoder_sdk::ImageFormat::kBGRA;
        else if (incaseStr == QStringLiteral("NV12"))
            return decoder_sdk::ImageFormat::kNV12;
        else if (incaseStr == QStringLiteral("NV21"))
            return decoder_sdk::ImageFormat::kNV21;

        return decoder_sdk::ImageFormat::kYUV420P;
    }

    decoder_sdk::HWAccelType transHwAccelTypeString(const QString &str)
    {
        const QString incaseStr = str.toUpper();
        if (incaseStr == QStringLiteral("NONE"))
            return decoder_sdk::HWAccelType::kNone;
        else if (incaseStr == QStringLiteral("AUTO"))
            return decoder_sdk::HWAccelType::kAuto;
        else if (incaseStr == QStringLiteral("CUDA"))
            return decoder_sdk::HWAccelType::kCuda;
        else if (incaseStr == QStringLiteral("DXVA2"))
            return decoder_sdk::HWAccelType::kDxva2;
        else if (incaseStr == QStringLiteral("D3D11VA"))
            return decoder_sdk::HWAccelType::kD3d11va;
        else if (incaseStr == QStringLiteral("VAAPI"))
            return decoder_sdk::HWAccelType::kVaapi;
        else if (incaseStr == QStringLiteral("VULKAN"))
            return decoder_sdk::HWAccelType::kVulkan;
        else if (incaseStr == QStringLiteral("QSV"))
            return decoder_sdk::HWAccelType::kQsv;
        else if (incaseStr == QStringLiteral("AMF"))
            return decoder_sdk::HWAccelType::kAmf;

        return decoder_sdk::HWAccelType::kAuto;
    }

    decoder_sdk::MediaTypes transRequiredMediaTypeString(const QString &str)
    {
        const QString incaseStr = str.toUpper();
        if (incaseStr == QStringLiteral("VIDEO"))
            return decoder_sdk::MediaType::kVideo;
        else if (incaseStr == QStringLiteral("AUDIO"))
            return decoder_sdk::MediaType::kAudio;
        else if (incaseStr == QStringLiteral("ALL"))
            return decoder_sdk::MediaType::kAll;

        return decoder_sdk::MediaType::kVideo;
    }

    decoder_sdk::DecoderConfig::RtspTransport transRtspTransportString(const QString &str)
    {
        const QString incaseStr = str.toUpper();
        if (incaseStr == QStringLiteral("TCP"))
            return decoder_sdk::DecoderConfig::RtspTransport::kTcp;
        else if (incaseStr == QStringLiteral("UDP"))
            return decoder_sdk::DecoderConfig::RtspTransport::kUdp;
        else if (incaseStr == QStringLiteral("UDP_MULTICAST"))
            return decoder_sdk::DecoderConfig::RtspTransport::kUdpMulticast;

        return decoder_sdk::DecoderConfig::RtspTransport::kTcp;
    }

    void *createHwContextCallback(decoder_sdk::HWAccelType type)
    {
        if (type != decoder_sdk::HWAccelType::kVulkan)
            return nullptr;

        switch (type) {
#ifdef D3D11VA_AVAILABLE
            case decoder_sdk::HWAccelType::kD3d11va:
                return d3d11_utils::getD3D11Device().Get();
#endif

#ifdef DXVA2_AVAILABLE
            case decoder_sdk::HWAccelType::kDxva2:
                return dxva2_utils::getDXVA2DeviceManager().Get();
#endif

#ifdef CUDA_AVAILABLE
            case decoder_sdk::HWAccelType::kCuda:
                return cuda_utils::getCudaContext();
#endif

#ifdef VULKAN_AVAILABLE
            case decoder_sdk::HWAccelType::kVulkan:
                return vulkan_utils::getDeviceContext();
#endif

            default:
                break;
        }

        return nullptr;
    }

    void freeHwContextCallback(decoder_sdk::HWAccelType type, void *userHwContext)
    {
        switch (type) {
#ifdef CUDA_AVAILABLE
            case decoder_sdk::HWAccelType::kCuda:
                if (CUcontext cuContext = static_cast<CUcontext>(userHwContext); cuContext) {
                    cuda_utils::releaseContext();
                }
                break;
#endif
            default:
                break;
        }

        return;
    }

    QString getFilePathFromRecordEvent(const std::shared_ptr<decoder_sdk::EventArgs> &event)
    {
        QString filePath;
        if (auto *const recordEvent = dynamic_cast<decoder_sdk::RecordingEventArgs *>(event.get());
            recordEvent) {
            filePath = QString::fromLocal8Bit(recordEvent->outputPath.data(), static_cast<int>(recordEvent->outputPath.size()));
        }

        return filePath;
    }

    bool equal(double a, double b, double epsilon)
    {
        return std::fabs(a - b) < epsilon;
    }
    bool equal(float a, float b, double epsilon)
    {
        return std::fabs(a - b) < epsilon;
    }

    bool greater(double a, double b, double epsilon)
    {
        return a > b && std::fabs(a - b) > epsilon;
    }
    bool greater(float a, float b, double epsilon)
    {
        return a > b && std::fabs(a - b) > epsilon;
    }

    bool greaterAndEqual(double a, double b, double epsilon)
    {
        return greater(a, b, epsilon) || equal(a, b, epsilon);
    }
    bool greaterAndEqual(float a, float b, double epsilon)
    {
        return greater(a, b, epsilon) || equal(a, b, epsilon);
    }
} // namespace utils