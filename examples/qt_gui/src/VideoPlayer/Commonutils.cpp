#include "Commonutils.h"

#include "decodersdk/frame.h"

#include <QDebug>
#include <QVariant>

#if defined(WIN32)
#include <Windows.h>
#endif

namespace {
const QString kOk = QStringLiteral("OK");
const QString kFail = QStringLiteral("FAIL");
} // namespace

void registerVideoMetaType()
{
    qRegisterMetaType<decoder_sdk::Frame>("decoder_sdk::Frame");
    qRegisterMetaType<std::shared_ptr<decoder_sdk::Frame>>("std::shared_ptr<decoder_sdk::Frame>");
    qRegisterMetaType<decoder_sdk::Config>("decoder_sdk::Config");
    qRegisterMetaType<std::shared_ptr<decoder_sdk::EventArgs>>(
        "std::shared_ptr<decoder_sdk::EventArgs>");
    qRegisterMetaType<decoder_sdk::EventType>("decoder_sdk::EventType");
}

void clearGPUResource()
{
#ifdef D3D11VA_AVAILABLE
    d3d11_utils::shutdown();
#endif

#ifdef DXVA2_AVAILABLE
    dxva2_utils::shutdown();
#endif
}

#ifdef CUDA_AVAILABLE
#include <mutex>

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

namespace cuda_utils {
class CudaManager {
public:
    static CudaManager &getInstance()
    {
        static CudaManager instance;
        return instance;
    }

    CUcontext getContext()
    {
        int deviceActived = 0;
        unsigned int deviceFlags = 0;
        const unsigned int desiredFlags = CU_CTX_SCHED_BLOCKING_SYNC;

        if (!ck(cuDevicePrimaryCtxGetState(device_, &deviceFlags, &deviceActived))) {
            qWarning() << QStringLiteral("Failed to get CUDA device state!");
            return nullptr;
        }

        if (deviceActived && deviceFlags != desiredFlags) {
            qWarning() << QStringLiteral(
                "CUDA Primary context already active with incompatible flags!");
        } else if (deviceFlags != desiredFlags) {
            if (!ck(cuDevicePrimaryCtxSetFlags(device_, desiredFlags))) {
                qWarning() << QStringLiteral("Failed to set CUDA device primary context flags!");
            }
        }

        CUcontext context = nullptr;
        cuDevicePrimaryCtxRetain(&context, device_);
        return context;
    }

    void releaseContext()
    {
        if (!device_)
            return;

        cuDevicePrimaryCtxRelease(device_);
    }

    CUdevice getDevice()
    {
        return device_;
    }

    bool isInitialized() const
    {
        return isInitialized_.load();
    }

    // 禁止拷贝和赋值
    CudaManager(const CudaManager &) = delete;
    CudaManager &operator=(const CudaManager &) = delete;

private:
    CudaManager()
    {
        initialize();
    }

    ~CudaManager()
    {
    }

    void initialize()
    {
        if (isInitialized_.load())
            return;

        isInitialized_.store(cuInit(0) == CUDA_SUCCESS && cuDeviceGet(&device_, 0) == CUDA_SUCCESS);
        if (isInitialized_.load()) {
            qDebug() << QStringLiteral("CUDA initialized successfully!");
        } else {
            qWarning() << QStringLiteral("CUDA initialized failed!");
        }
    }

    CUdevice device_ = 0;
    std::atomic_bool isInitialized_ = false;
};

// 全局访问函数
CUcontext getCudaContext()
{
    return CudaManager::getInstance().getContext();
}

void releaseContext()
{
    return CudaManager::getInstance().releaseContext();
}

CUdevice getCudaDevice()
{
    return CudaManager::getInstance().getDevice();
}

bool isCudaAvailable()
{
    return CudaManager::getInstance().isInitialized();
}
} // namespace cuda_utils
#endif

#if defined(DXVA2_AVAILABLE) || defined(D3D11VA_AVAILABLE)
namespace wgl {
// 在现有的WGL函数指针定义中添加
typedef BOOL(WINAPI *PFNWGLDXSETRESOURCESHAREHANDLENVPROC)(void *dxObject, HANDLE shareHandle);
typedef HANDLE(WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dxDevice);
typedef BOOL(WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE hDevice, void *dxObject, GLuint name,
                                                     GLenum type, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE hDevice, GLint count, HANDLE *hObjects);

struct FuncTable {
    PFNWGLDXSETRESOURCESHAREHANDLENVPROC wglDXSetResourceShareHandleNV = nullptr;
    PFNWGLDXOPENDEVICENVPROC wglDXOpenDeviceNV = nullptr;
    PFNWGLDXCLOSEDEVICENVPROC wglDXCloseDeviceNV = nullptr;
    PFNWGLDXREGISTEROBJECTNVPROC wglDXRegisterObjectNV = nullptr;
    PFNWGLDXUNREGISTEROBJECTNVPROC wglDXUnregisterObjectNV = nullptr;
    PFNWGLDXLOCKOBJECTSNVPROC wglDXLockObjectsNV = nullptr;
    PFNWGLDXUNLOCKOBJECTSNVPROC wglDXUnlockObjectsNV = nullptr;
};
static struct FuncTable g_funcTable;
static bool g_funcTableLoaded = false;

// WglDeviceRef::ControlBlock 实现
WglDeviceRef::ControlBlock::ControlBlock(HANDLE handle) : wglHandle(handle)
{
}

WglDeviceRef::ControlBlock::~ControlBlock()
{
    if (wglHandle && g_funcTable.wglDXCloseDeviceNV) {
        g_funcTable.wglDXCloseDeviceNV(wglHandle);
        wglHandle = nullptr;
    }
}

// WglDeviceRef 实现
WglDeviceRef::WglDeviceRef(void *dxObject)
{
    if (!dxObject) {
        return;
    }

    HANDLE wglHandle = createWglDevice(dxObject);
    if (wglHandle) {
        control_ = new ControlBlock(wglHandle);
    }
}

WglDeviceRef::WglDeviceRef(const WglDeviceRef &other) noexcept
{
    acquire(other.control_);
}

WglDeviceRef &WglDeviceRef::operator=(const WglDeviceRef &other) noexcept
{
    if (control_ == other.control_) {
        return *this;
    }
    release();
    acquire(other.control_);
    return *this;
}

WglDeviceRef::WglDeviceRef(WglDeviceRef &&other) noexcept
{
    control_ = other.control_;
    other.control_ = nullptr;
}

WglDeviceRef &WglDeviceRef::operator=(WglDeviceRef &&other) noexcept
{
    if (control_ == other.control_) {
        return *this;
    }
    release();
    control_ = other.control_;
    other.control_ = nullptr;
    return *this;
}

WglDeviceRef::~WglDeviceRef()
{
    release();
}

HANDLE WglDeviceRef::get() const noexcept
{
    return control_ ? control_->wglHandle : nullptr;
}

void WglDeviceRef::reset(HANDLE new_handle)
{
    release();
    if (new_handle) {
        control_ = new ControlBlock(new_handle);
    }
}

int WglDeviceRef::use_count() const noexcept
{
    return control_ ? control_->refCount.load(std::memory_order_relaxed) : 0;
}

bool WglDeviceRef::isValid() const noexcept
{
    return control_ && control_->wglHandle;
}

WglDeviceRef::operator bool() const noexcept
{
    return get() != nullptr;
}

bool WglDeviceRef::operator==(const WglDeviceRef &other) const noexcept
{
    return get() == other.get();
}

bool WglDeviceRef::operator!=(const WglDeviceRef &other) const noexcept
{
    return !(*this == other);
}

HANDLE WglDeviceRef::wglDXRegisterObjectNV(void *dxObject, GLuint name, GLenum type, GLenum access)
{
    if (!control_ || !control_->wglHandle) {
        return nullptr;
    }

    HANDLE hObject =
        g_funcTable.wglDXRegisterObjectNV(control_->wglHandle, dxObject, name, type, access);
    if (!hObject) {
        qWarning() << QStringLiteral("Failed to register WGL object");
    }
    return hObject;
}

BOOL WglDeviceRef::wglDXUnregisterObjectNV(HANDLE hObject)
{
    if (!control_ || !control_->wglHandle) {
        return FALSE;
    }

    BOOL ret = g_funcTable.wglDXUnregisterObjectNV(control_->wglHandle, hObject);
    if (!ret) {
        qWarning() << QStringLiteral("Failed to unregister WGL object");
    }
    return ret;
}

BOOL WglDeviceRef::wglDXLockObjectsNV(GLint count, HANDLE *hObjects)
{
    if (!control_ || !control_->wglHandle) {
        return FALSE;
    }
    BOOL ret = g_funcTable.wglDXLockObjectsNV(control_->wglHandle, count, hObjects);
    if (!ret) {
        qWarning() << QStringLiteral("Failed to lock WGL objects");
    }
    return ret;
}

BOOL WglDeviceRef::wglDXUnlockObjectsNV(GLint count, HANDLE *hObjects)
{
    if (!control_ || !control_->wglHandle) {
        return FALSE;
    }
    BOOL ret = g_funcTable.wglDXUnlockObjectsNV(control_->wglHandle, count, hObjects);
    if (!ret) {
        qWarning() << QStringLiteral("Failed to unlock WGL objects");
    }
    return ret;
}

void WglDeviceRef::acquire(ControlBlock *ctrl) noexcept
{
    control_ = ctrl;
    if (control_) {
        control_->refCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void WglDeviceRef::release() noexcept
{
    if (control_ && control_->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete control_;
    }
    control_ = nullptr;
}

HANDLE WglDeviceRef::createWglDevice(void *device)
{
    if (!device || !g_funcTable.wglDXOpenDeviceNV) {
        return nullptr;
    }

    HANDLE wglHandle = g_funcTable.wglDXOpenDeviceNV(device);
    if (!wglHandle) {
        DWORD error = GetLastError();
        qWarning()
            << QStringLiteral("Failed to open WGL device, error: %1").arg(QString::number(error));
    }

    return wglHandle;
}

bool loadFuncTable()
{
    if (g_funcTableLoaded) {
        return true;
    }

    // 加载函数指针
    g_funcTable.wglDXOpenDeviceNV =
        (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
    g_funcTable.wglDXCloseDeviceNV =
        (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
    g_funcTable.wglDXSetResourceShareHandleNV =
        (PFNWGLDXSETRESOURCESHAREHANDLENVPROC)wglGetProcAddress("wglDXSetResourceShareHandleNV");
    g_funcTable.wglDXRegisterObjectNV =
        (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXRegisterObjectNV");
    g_funcTable.wglDXUnregisterObjectNV =
        (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress("wglDXUnregisterObjectNV");
    g_funcTable.wglDXLockObjectsNV =
        (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
    g_funcTable.wglDXUnlockObjectsNV =
        (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");

    const bool success = g_funcTable.wglDXOpenDeviceNV && g_funcTable.wglDXCloseDeviceNV &&
                         g_funcTable.wglDXSetResourceShareHandleNV &&
                         g_funcTable.wglDXRegisterObjectNV && g_funcTable.wglDXUnregisterObjectNV &&
                         g_funcTable.wglDXLockObjectsNV && g_funcTable.wglDXUnlockObjectsNV;

    if (!success) {
        qWarning() << QStringLiteral("Failed to load WGL function pointers:");
        qWarning() << QStringLiteral("wglDXOpenDeviceNV: %1")
                          .arg((g_funcTable.wglDXOpenDeviceNV ? kOk : kFail));
        qWarning() << QStringLiteral("wglDXCloseDeviceNV: %1")
                          .arg((g_funcTable.wglDXCloseDeviceNV ? kOk : kFail));
        qWarning() << QStringLiteral("wglDXSetResourceShareHandleNV: %1")
                          .arg((g_funcTable.wglDXSetResourceShareHandleNV ? kOk : kFail));
        qWarning() << QStringLiteral("wglDXRegisterObjectNV: %1")
                          .arg((g_funcTable.wglDXRegisterObjectNV ? kOk : kFail));
        qWarning() << QStringLiteral("wglDXUnregisterObjectNV: %1")
                          .arg((g_funcTable.wglDXUnregisterObjectNV ? kOk : kFail));
        qWarning() << QStringLiteral("wglDXLockObjectsNV: %1")
                          .arg((g_funcTable.wglDXLockObjectsNV ? kOk : kFail));
        qWarning() << QStringLiteral("wglDXUnlockObjectsNV: %1")
                          .arg((g_funcTable.wglDXUnlockObjectsNV ? kOk : kFail));
    }

    g_funcTableLoaded = true;
    return success;
}

BOOL wglDXSetResourceShareHandleNV(void *dxObject, HANDLE shareHandle)
{
    if (!g_funcTable.wglDXSetResourceShareHandleNV) {
        qCritical() << QStringLiteral("Can not get wglDXSetResourceShareHandleNV proc address!");
        return FALSE;
    }

    return g_funcTable.wglDXSetResourceShareHandleNV(dxObject, shareHandle);
}

HANDLE wglDXOpenDeviceNV(void *dxDevice)
{
    if (!g_funcTable.wglDXOpenDeviceNV) {
        qCritical() << QStringLiteral("Can not get wglDXOpenDeviceNV proc address!");
        return nullptr;
    }

    return g_funcTable.wglDXOpenDeviceNV(dxDevice);
}

BOOL wglDXCloseDeviceNV(HANDLE hDevice)
{
    if (!g_funcTable.wglDXCloseDeviceNV) {
        qCritical() << QStringLiteral("Can not get wglDXCloseDeviceNV proc address!");
        return FALSE;
    }

    return g_funcTable.wglDXCloseDeviceNV(hDevice);
}

HANDLE wglDXRegisterObjectNV(HANDLE hDevice, void *dxObject, GLuint name, GLenum type,
                             GLenum access)
{
    if (!g_funcTable.wglDXRegisterObjectNV) {
        qCritical() << QStringLiteral("Can not get wglDXRegisterObjectNV proc address!");
        return FALSE;
    }

    return g_funcTable.wglDXRegisterObjectNV(hDevice, dxObject, name, type, access);
}

BOOL wglDXUnregisterObjectNV(HANDLE hDevice, HANDLE hObject)
{
    if (!g_funcTable.wglDXUnregisterObjectNV) {
        qCritical() << QStringLiteral("Can not get wglDXUnregisterObjectNV proc address!");
        return FALSE;
    }

    return g_funcTable.wglDXUnregisterObjectNV(hDevice, hObject);
}

BOOL wglDXLockObjectsNV(HANDLE hDevice, GLint count, HANDLE *hObjects)
{
    if (!g_funcTable.wglDXLockObjectsNV) {
        qCritical() << QStringLiteral("Can not get wglDXLockObjectsNV proc address!");
        return FALSE;
    }

    return g_funcTable.wglDXLockObjectsNV(hDevice, count, hObjects);
}

BOOL wglDXUnlockObjectsNV(HANDLE hDevice, GLint count, HANDLE *hObjects)
{
    if (!g_funcTable.wglDXUnlockObjectsNV) {
        qCritical() << QStringLiteral("Can not get wglDXUnlockObjectsNV proc address!");
        return FALSE;
    }

    return g_funcTable.wglDXUnlockObjectsNV(hDevice, count, hObjects);
}
} // namespace wgl
#endif

#ifdef D3D11VA_AVAILABLE
#include <mutex>

namespace d3d11_utils {
class D3D11Manager {
public:
    static D3D11Manager &getInstance()
    {
        static D3D11Manager instance;
        return instance;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> getDevice()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return device_;
    }

    wgl::WglDeviceRef getWglDeviceRef()
    {
        std::call_once(initWglFlag_, [this]() { initializeWgl(); });
        return wglD3DDevice_;
    }

    bool isInitialized() const
    {
        return device_ != nullptr;
    }

    void shutdown()
    {
        if (wglD3DDevice_) {
            wglD3DDevice_.reset();
        }

        if (device_) {
            /*Microsoft::WRL::ComPtr<ID3D11Debug> debug;
            device_->QueryInterface(__uuidof(ID3D11Debug), (void **)&debug);*/
            device_.Reset();
        }
    }

    // 禁止拷贝和赋值
    D3D11Manager(const D3D11Manager &) = delete;
    D3D11Manager &operator=(const D3D11Manager &) = delete;

private:
    D3D11Manager() = default;
    ~D3D11Manager()
    {
        shutdown();
    }

    void initialize()
    {
        UINT createDeviceFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        // #ifdef _DEBUG
        //             createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
        // #endif

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
            qWarning() << QStringLiteral("D3D11 device initialization failed, HRESULT:") << Qt::hex
                       << hr;
            device_.Reset();
        }
    }

    void initializeWgl()
    {
        if (!wgl::loadFuncTable()) {
            qWarning() << QStringLiteral("Failed to load WGL function table!");
        }

        if (!device_) {
            getDevice();
        }

        // 得到WGL互操作设备
        wglD3DDevice_ = wgl::WglDeviceRef(device_.Get());
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    wgl::WglDeviceRef wglD3DDevice_; // WGL设备句柄
    std::once_flag initFlag_;
    std::once_flag initWglFlag_;
};

// 全局访问函数
Microsoft::WRL::ComPtr<ID3D11Device> getD3D11Device()
{
    return D3D11Manager::getInstance().getDevice();
}

wgl::WglDeviceRef getWglDeviceRef()
{
    return D3D11Manager::getInstance().getWglDeviceRef();
}

bool isD3D11Available()
{
    return D3D11Manager::getInstance().isInitialized();
}

void shutdown()
{
    return D3D11Manager::getInstance().shutdown();
}
} // namespace d3d11_utils
#endif

#ifdef DXVA2_AVAILABLE
#include <mutex>

namespace dxva2_utils {
class DXVA2Manager {
public:
    static DXVA2Manager &getInstance()
    {
        static DXVA2Manager instance;
        return instance;
    }

    Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> getDeviceManager()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return deviceManager_;
    }

    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> getDevice()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return device_;
    }

    wgl::WglDeviceRef getWglDeviceRef()
    {
        std::call_once(initWglFlag_, [this]() { initializeWgl(); });
        return wglD3DDevice_;
    }

    bool isInitialized() const
    {
        return deviceManager_ != nullptr;
    }

    void shutdown()
    {
        if (wglD3DDevice_) {
            wglD3DDevice_.reset();
        }
        if (device_) {
            device_.Reset();
        }
        if (deviceManager_) {
            deviceManager_.Reset();
        }
    }

    // 禁止拷贝和赋值
    DXVA2Manager(const DXVA2Manager &) = delete;
    DXVA2Manager &operator=(const DXVA2Manager &) = delete;

private:
    DXVA2Manager() = default;
    ~DXVA2Manager()
    {
        shutdown();
    }

    void initialize()
    {
        // 创建Direct3D9对象
        Microsoft::WRL::ComPtr<IDirect3D9Ex> d3d9ex;
        Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9ex);
        if (!d3d9ex) {
            qWarning() << QStringLiteral("Failed to create Direct3D9 object");
            return;
        }

        // 获取默认适配器信息
        D3DADAPTER_IDENTIFIER9 adapterInfo;
        HRESULT hr = d3d9ex->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapterInfo);
        if (FAILED(hr)) {
            qWarning() << QStringLiteral("Failed to get adapter identifier, HRESULT:") << Qt::hex
                       << hr;
            return;
        }
        qInfo() << QStringLiteral("D3D9Ex DeviceName: %1, DeviceIndex: %2, Description: %3")
                       .arg(adapterInfo.DeviceName, QString::number(adapterInfo.DeviceId),
                            adapterInfo.Description);

        D3DDISPLAYMODEEX modeex = {0};
        modeex.Size = sizeof(D3DDISPLAYMODEEX);
        d3d9ex->GetAdapterDisplayModeEx(D3DADAPTER_DEFAULT, &modeex, NULL);
        if (FAILED(hr)) {
            d3d9ex->Release();
            qWarning() << QStringLiteral("Failed to get display mode, HRESULT:") << Qt::hex << hr;
            return;
        }

        // 创建Direct3D9设备
        D3DPRESENT_PARAMETERS presentParams = {};
        presentParams.Windowed = TRUE;
        presentParams.BackBufferWidth = 1;
        presentParams.BackBufferHeight = 1;
        presentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
        presentParams.BackBufferFormat = modeex.Format;
        presentParams.BackBufferCount = 0;
        presentParams.Flags = D3DPRESENTFLAG_VIDEO;
        presentParams.hDeviceWindow = NULL;

        // 添加多线程支持标志
        hr = d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE |
                                        D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                        D3DCREATE_DISABLE_PSGP_THREADING | D3DCREATE_MULTITHREADED,
                                    &presentParams, nullptr, &device_);

        if (FAILED(hr)) {
            qWarning() << QStringLiteral("Failed to create Direct3D9 device, HRESULT:") << Qt::hex
                       << hr;
            return;
        }

        // Check if it's possible to StretchRect() from NV12 to XRGB surfaces
        hr = d3d9ex->CheckDeviceFormatConversion(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                                 D3DFORMAT(MAKEFOURCC('N', 'V', '1', '2')),
                                                 D3DFMT_X8R8G8B8);
        if (hr != S_OK) {
            qWarning() << QStringLiteral("Can't StretchRect from NV12 to XRGB surfaces, HRESULT:")
                       << Qt::hex << hr;
            d3d9ex.Reset();
            return;
        }

        // 创建DXVA2设备管理器
        UINT resetToken = 0;
        hr = DXVA2CreateDirect3DDeviceManager9(&resetToken, &deviceManager_);
        if (FAILED(hr)) {
            qWarning() << QStringLiteral("Failed to create DXVA2 device manager, HRESULT:")
                       << Qt::hex << hr;
            return;
        }

        // 重置设备管理器
        hr = deviceManager_->ResetDevice(device_.Get(), resetToken);
        if (FAILED(hr)) {
            qWarning() << QStringLiteral("Failed to reset DXVA2 device manager, HRESULT:")
                       << Qt::hex << hr;
            deviceManager_.Reset();
            return;
        }

        qInfo() << QStringLiteral(
            "DXVA2 device manager initialized successfully with multithread support");
    }

    void initializeWgl()
    {
        if (!wgl::loadFuncTable()) {
            qWarning() << QStringLiteral("Failed to load WGL function table!");
        }

        if (!device_) {
            getDevice();
        }

        // 得到WGL互操作设备
        wglD3DDevice_ = wgl::WglDeviceRef(device_.Get());
    }

    Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> deviceManager_;
    Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> device_;
    wgl::WglDeviceRef wglD3DDevice_; // WGL设备句柄
    std::once_flag initFlag_;
    std::once_flag initWglFlag_;
};

// 全局访问函数
Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> getDXVA2DeviceManager()
{
    return DXVA2Manager::getInstance().getDeviceManager();
}

Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> getDXVA2Device()
{
    return DXVA2Manager::getInstance().getDevice();
}

wgl::WglDeviceRef getWglDeviceRef()
{
    return DXVA2Manager::getInstance().getWglDeviceRef();
}

bool isDXVA2Available()
{
    return DXVA2Manager::getInstance().isInitialized();
}

void shutdown()
{
    return DXVA2Manager::getInstance().shutdown();
}
} // namespace dxva2_utils
#endif

#ifdef VAAPI_AVAILABLE
#include <mutex>

#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace egl {
struct FuncTable {
    PFNEGLCREATEIMAGEKHRPROC egl_create_image_KHR;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image_KHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_egl_image_target_texture2d_oes;
};
static struct FuncTable g_funcTable;

bool loadFuncTable()
{
    g_funcTable.egl_create_image_KHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    if (!g_funcTable.egl_create_image_KHR) {
        qCritical() << QStringLiteral("Can not get eglCreateImageKHR proc address!");
        return false;
    }
    g_funcTable.egl_destroy_image_KHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    if (!g_funcTable.egl_destroy_image_KHR) {
        qCritical() << QStringLiteral("Can not get eglDestroyImageKHR proc address!");
        return false;
    }
    g_funcTable.gl_egl_image_target_texture2d_oes =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!g_funcTable.gl_egl_image_target_texture2d_oes) {
        qCritical() << QStringLiteral("Can not get glEGLImageTargetTexture2DOES proc address!");
        return false;
    }

    return true;
}

EGLImageKHR egl_create_image_KHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                 EGLClientBuffer buffer, const EGLint *attrib_list)
{
    if (!g_funcTable.egl_create_image_KHR) {
        qCritical() << QStringLiteral("Can not get eglCreateImageKHR proc address!");
        return nullptr;
    }

    return g_funcTable.egl_create_image_KHR(dpy, ctx, target, buffer, attrib_list);
}

EGLBoolean egl_destroy_image_KHR(EGLDisplay dpy, EGLImageKHR image)
{
    if (!g_funcTable.egl_destroy_image_KHR) {
        qCritical() << QStringLiteral("Can not get egl_destroy_image_KHR proc address!");
        return 0;
    }

    return g_funcTable.egl_destroy_image_KHR(dpy, image);
}

void gl_egl_image_target_texture2d_oes(GLenum target, GLeglImageOES image)
{
    if (!g_funcTable.gl_egl_image_target_texture2d_oes) {
        qCritical() << QStringLiteral("Can not get glEGLImageTargetTexture2DOES proc address!");
        return;
    }

    g_funcTable.gl_egl_image_target_texture2d_oes(target, image);
}
} // namespace egl

namespace vaapi_utils {
class VADisplayManager {
public:
    static VADisplayManager &getInstance()
    {
        static VADisplayManager instance;
        return instance;
    }

    VADisplay getVADisplay()
    {
        std::call_once(init_flag_, [this]() { initialize(); });
        return vaDisplay_;
    }

    bool isInitialized() const
    {
        return vaDisplay_ != nullptr;
    }

    // 禁止拷贝和赋值
    VADisplayManager(const VADisplayManager &) = delete;
    VADisplayManager &operator=(const VADisplayManager &) = delete;

private:
    VADisplayManager() = default;
    ~VADisplayManager()
    {
        if (isInitialized()) {
            decoder_sdk::destoryDrmVADisplay(vaDisplay_, fd_);
        }
    }

    void initialize()
    {
        egl::loadFuncTable();

        vaDisplay_ = decoder_sdk::createDrmVADisplay(fd_);
        if (vaDisplay_ == nullptr) {
            qWarning() << QStringLiteral("VADisplay initialize failed!");
        } else {
            qDebug() << QStringLiteral("VADisplay initialize successful!");
        }
    }

    VADisplay vaDisplay_ = nullptr;
    int fd_ = -1;
    std::once_flag init_flag_;
};

// 全局访问函数
VADisplay getVADisplayDRM()
{
    return VADisplayManager::getInstance().getVADisplay();
}

bool isVAAPIAvailable()
{
    return VADisplayManager::getInstance().isInitialized();
}
} // namespace vaapi_utils
#endif