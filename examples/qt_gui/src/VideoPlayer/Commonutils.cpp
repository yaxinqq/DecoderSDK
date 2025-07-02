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
    qRegisterMetaType<std::shared_ptr<decoder_sdk::EventArgs>>(
        "std::shared_ptr<decoder_sdk::EventArgs>");
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

#ifdef D3D11VA_AVAILABLE
#include <QDebug>
#include <mutex>

namespace D3D11Utils {
class D3D11Manager {
public:
    static D3D11Manager &getInstance()
    {
        static D3D11Manager instance;
        return instance;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> getDevice()
    {
        std::call_once(init_flag_, [this]() { initialize(); });
        return device_;
    }

    bool isInitialized() const
    {
        return device_ != nullptr;
    }

    // 禁止拷贝和赋值
    D3D11Manager(const D3D11Manager &) = delete;
    D3D11Manager &operator=(const D3D11Manager &) = delete;

private:
    D3D11Manager() = default;
    ~D3D11Manager() = default;

    void initialize()
    {
        UINT createDeviceFlags = 0;
#ifdef _DEBUG
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };

        D3D_FEATURE_LEVEL featureLevel;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

        HRESULT hr = D3D11CreateDevice(nullptr,                  // 使用默认适配器
                                       D3D_DRIVER_TYPE_HARDWARE, // 硬件驱动
                                       nullptr,                  // 软件驱动句柄
                                       createDeviceFlags,        // 创建标志
                                       featureLevels,            // 特性级别数组
                                       ARRAYSIZE(featureLevels), // 特性级别数组大小
                                       D3D11_SDK_VERSION,        // SDK版本
                                       &device_,                 // 输出设备
                                       &featureLevel,            // 输出特性级别
                                       &context                  // 输出设备上下文
        );

        if (SUCCEEDED(hr)) {
            // 获取 multithread 接口
            ID3D10Multithread *multithread = nullptr;
            context->QueryInterface(__uuidof(ID3D10Multithread), (void **)&multithread);

            if (multithread) {
                multithread->SetMultithreadProtected(TRUE); // 开启多线程保护
                multithread->Release();
            }
        } else {
            qDebug() << "D3D11 device initialization failed, HRESULT:" << Qt::hex << hr;
            device_.Reset();
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    std::once_flag init_flag_;
};

// 全局访问函数
Microsoft::WRL::ComPtr<ID3D11Device> getD3D11Device()
{
    return D3D11Manager::getInstance().getDevice();
}

bool isD3D11Available()
{
    return D3D11Manager::getInstance().isInitialized();
}
} // namespace D3D11Utils
#endif

#ifdef DXVA2_AVAILABLE
#include <QDebug>
#include <mutex>

namespace DXVA2Utils {
class DXVA2Manager {
public:
    static DXVA2Manager &getInstance()
    {
        static DXVA2Manager instance;
        return instance;
    }

    Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> getDeviceManager()
    {
        std::call_once(init_flag_, [this]() { initialize(); });
        return deviceManager_;
    }

    bool isInitialized() const
    {
        return deviceManager_ != nullptr;
    }

    // 禁止拷贝和赋值
    DXVA2Manager(const DXVA2Manager &) = delete;
    DXVA2Manager &operator=(const DXVA2Manager &) = delete;

private:
    DXVA2Manager() = default;
    ~DXVA2Manager() = default;

    void initialize()
    {
        // 创建Direct3D9对象
        Microsoft::WRL::ComPtr<IDirect3D9> d3d9;
        d3d9.Attach(Direct3DCreate9(D3D_SDK_VERSION));
        if (!d3d9) {
            qDebug() << "Failed to create Direct3D9 object";
            return;
        }

        // 获取默认适配器信息
        D3DADAPTER_IDENTIFIER9 adapterInfo;
        HRESULT hr = d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapterInfo);
        if (FAILED(hr)) {
            qDebug() << "Failed to get adapter identifier, HRESULT:" << Qt::hex << hr;
            return;
        }

        // 创建Direct3D9设备
        D3DPRESENT_PARAMETERS presentParams = {};
        presentParams.Windowed = TRUE;
        presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
        presentParams.BackBufferFormat = D3DFMT_UNKNOWN;
        presentParams.hDeviceWindow = GetDesktopWindow();

        Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
        // 添加多线程支持标志
        hr = d3d9->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE,
            &presentParams, &device);

        if (FAILED(hr)) {
            qDebug() << "Failed to create Direct3D9 device, HRESULT:" << Qt::hex << hr;
            return;
        }

        // 创建DXVA2设备管理器
        UINT resetToken = 0;
        hr = DXVA2CreateDirect3DDeviceManager9(&resetToken, &deviceManager_);
        if (FAILED(hr)) {
            qDebug() << "Failed to create DXVA2 device manager, HRESULT:" << Qt::hex << hr;
            return;
        }

        // 重置设备管理器
        hr = deviceManager_->ResetDevice(device.Get(), resetToken);
        if (FAILED(hr)) {
            qDebug() << "Failed to reset DXVA2 device manager, HRESULT:" << Qt::hex << hr;
            deviceManager_.Reset();
            return;
        }

        qDebug() << "DXVA2 device manager initialized successfully with multithread support";
    }

    Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> deviceManager_;
    std::once_flag init_flag_;
};

// 全局访问函数
Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> getDXVA2DeviceManager()
{
    return DXVA2Manager::getInstance().getDeviceManager();
}

bool isDXVA2Available()
{
    return DXVA2Manager::getInstance().isInitialized();
}
} // namespace DXVA2Utils
#endif