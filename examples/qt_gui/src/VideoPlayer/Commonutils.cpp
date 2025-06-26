#include "Commonutils.h"

#include "decodersdk/frame.h"

#include <QVariant>

#if defined(WIN32)
#include <Windows.h>
#endif

void registerVideoMetaType()
{
    qRegisterMetaType<decoder_sdk::Frame>("decoder_sdk::Frame");
    qRegisterMetaType<decoder_sdk::Config>("decoder_sdk::Config");
    qRegisterMetaType<std::shared_ptr<decoder_sdk::EventArgs>>("std::shared_ptr<decoder_sdk::EventArgs>");
    qRegisterMetaType<decoder_sdk::EventType>("decoder_sdk::EventType");
}

#ifdef CUDA_AVAILABLE
#include <mutex>

#include <QDebug>
namespace {
inline bool check(int e, int iLine, const char *szFile)
{
    if (e != 0) {
        const char *errstr = NULL;
        cuGetErrorString(static_cast<CUresult>(e), &errstr);
        qDebug() << "General error " << e << " error string: " << errstr << " at line " << iLine
                 << " in file " << szFile;
        return false;
    }
    return true;
}

#define ck(call) check(call, __LINE__, __FILE__)
} // namespace

namespace CudaUtils {
class CudaManager {
public:
    static CudaManager &getInstance()
    {
        static CudaManager instance;
        return instance;
    }

    CUcontext getContext()
    {
        std::call_once(init_flag_, [this]() { initialize(); });
        return context_;
    }

    bool isInitialized() const
    {
        return context_ != nullptr;
    }

    // 禁止拷贝和赋值
    CudaManager(const CudaManager &) = delete;
    CudaManager &operator=(const CudaManager &) = delete;

private:
    CudaManager() = default;

    ~CudaManager()
    {
        if (context_) {
            // 注意：析构函数中不应该调用可能失败的CUDA函数
            // 但为了资源清理，这里还是需要调用
            cuDevicePrimaryCtxRelease(device_);
        }
    }

    void initialize()
    {
        if (cuInit(0) == CUDA_SUCCESS && cuDeviceGet(&device_, 0) == CUDA_SUCCESS &&
            cuDevicePrimaryCtxRetain(&context_, device_) == CUDA_SUCCESS) {
            // 初始化成功
            qDebug() << "CUDA initialized successfully";
        } else {
            qDebug() << "CUDA initialization failed";
            context_ = nullptr;
        }
    }

    CUcontext context_ = nullptr;
    CUdevice device_ = 0;
    std::once_flag init_flag_;
};

// 全局访问函数
CUcontext getCudaContext()
{
    return CudaManager::getInstance().getContext();
}

bool isCudaAvailable()
{
    return CudaManager::getInstance().isInitialized();
}
} // namespace CudaUtils
#endif