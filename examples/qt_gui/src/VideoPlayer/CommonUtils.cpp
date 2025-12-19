#include "CommonUtils.h"
#include "StreamManager.h"

#include "decodersdk/decoder_sdk_def.h"
#include "decodersdk/frame.h"

#include <QApplication>
#include <QDebug>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QVariant>
#include <QtGlobal>

#ifdef Q_OS_WINDOWS
#include <Windows.h>
#else
#include <dlfcn.h>
#define GetProcAddress dlsym
#define FreeLibrary dlclose
#endif

namespace {
const QString kOk = QStringLiteral("OK");
const QString kFail = QStringLiteral("FAIL");
static QString kGlRenderer;

QString getGLRenderer()
{
    // 查找当前OpenGL Context所绑定的设备
    QOpenGLContext glContext;
    glContext.create();

    QOffscreenSurface glOffscreenSurface;
    glOffscreenSurface.create();

    glContext.makeCurrent(&glOffscreenSurface);
    QString glRenderer;
    if (glContext.isValid()) {
        glRenderer = QString(
            reinterpret_cast<const char *>(glContext.functions()->glGetString(GL_RENDERER)));
    }
    glContext.doneCurrent();

    // 输出
    if (glRenderer.isEmpty()) {
        qWarning() << QStringLiteral("Failed to get OpenGL Renderer!");
    } else {
        qInfo() << QStringLiteral("OpenGL Renderer: %1").arg(glRenderer);
    }

    return glRenderer;
}

#ifdef Q_OS_WINDOWS
HMODULE loadLibrary(const char *const dllName)
#else
void *loadLibrary(const char *const dllName)
#endif
{
#ifdef _WIN32
    // 首先尝试系统目录的 nvcuda.dll，避免目录搜索带来的 DLL 劫持
    HMODULE handle = nullptr;
    char sysPath[MAX_PATH] = {0};
    if (GetSystemDirectoryA(sysPath, MAX_PATH)) {
        const std::string fullPath = std::string(sysPath) + "\\" + dllName;
        handle = LoadLibraryA(fullPath.c_str());
    }
    if (!handle) {
        // 退回到默认加载（PATH）
        handle = LoadLibraryA(dllName);
    }
#else
    void *handle = nullptr;
    handle = dlopen(dllName, RTLD_LAZY);
    if (!handle) {
        qWarning() << QStringLiteral("Load error: %1").arg(dlerror());
    }
#endif

    return handle;
}
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

QString getCurrentGLRenderer()
{
    if (kGlRenderer.isEmpty()) {
        kGlRenderer = getGLRenderer();
    }

    return kGlRenderer;
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

namespace cuda_utils {
// CUDA 函数指针类型定义
typedef CUresult(CUDAAPI *PFN_cuInit)(unsigned int Flags);
typedef CUresult(CUDAAPI *PFN_cuCtxSetCurrent)(CUcontext ctx);
typedef CUresult(CUDAAPI *PFN_cuCtxGetCurrent)(CUcontext *pctx);
typedef CUresult(CUDAAPI *PFN_cuDeviceGetCount)(int *count);
typedef CUresult(CUDAAPI *PFN_cuDeviceGet)(CUdevice *device, int ordinal);
typedef CUresult(CUDAAPI *PFN_cuDeviceGetName)(char *name, int len, CUdevice dev);
typedef CUresult(CUDAAPI *PFN_cuDevicePrimaryCtxGetState)(CUdevice dev, unsigned int *flags,
                                                          int *active);
typedef CUresult(CUDAAPI *PFN_cuDevicePrimaryCtxSetFlags)(CUdevice dev, unsigned int flags);
typedef CUresult(CUDAAPI *PFN_cuDevicePrimaryCtxRetain)(CUcontext *pctx, CUdevice dev);
typedef CUresult(CUDAAPI *PFN_cuDevicePrimaryCtxRelease)(CUdevice dev);
typedef CUresult(CUDAAPI *PFN_cuStreamCreate)(CUstream *phStream, unsigned int Flags);
typedef CUresult(CUDAAPI *PFN_cuStreamDestroy)(CUstream hStream);
typedef CUresult(CUDAAPI *PFN_cuStreamAddCallback)(CUstream hStream, CUstreamCallback callback,
                                                   void *userData, unsigned int flags);
typedef CUresult(CUDAAPI *PFN_cuGraphicsGLRegisterImage)(CUgraphicsResource *pCudaResource,
                                                         unsigned int image, unsigned int target,
                                                         unsigned int Flags);
typedef CUresult(CUDAAPI *PFN_cuGraphicsUnregisterResource)(CUgraphicsResource resource);
typedef CUresult(CUDAAPI *PFN_cuGraphicsMapResources)(unsigned int count,
                                                      CUgraphicsResource *resources,
                                                      CUstream hStream);
typedef CUresult(CUDAAPI *PFN_cuGraphicsUnmapResources)(unsigned int count,
                                                        CUgraphicsResource *resources,
                                                        CUstream hStream);
typedef CUresult(CUDAAPI *PFN_cuGraphicsSubResourceGetMappedArray)(CUarray *pArray,
                                                                   CUgraphicsResource resource,
                                                                   unsigned int arrayIndex,
                                                                   unsigned int mipLevel);
typedef CUresult(CUDAAPI *PFN_cuMemcpy2DAsync)(const CUDA_MEMCPY2D *pCopy, CUstream hStream);
typedef CUresult(CUDAAPI *PFN_cuGetErrorString)(CUresult error, const char **pStr);

// CUDA 函数表
struct CudaFuncTable {
    PFN_cuInit cuInit = nullptr;
    PFN_cuCtxSetCurrent cuCtxSetCurrent = nullptr;
    PFN_cuCtxGetCurrent cuCtxGetCurrent = nullptr;
    PFN_cuDeviceGetCount cuDeviceGetCount = nullptr;
    PFN_cuDeviceGet cuDeviceGet = nullptr;
    PFN_cuDeviceGetName cuDeviceGetName = nullptr;
    PFN_cuDevicePrimaryCtxGetState cuDevicePrimaryCtxGetState = nullptr;
    PFN_cuDevicePrimaryCtxSetFlags cuDevicePrimaryCtxSetFlags = nullptr;
    PFN_cuDevicePrimaryCtxRetain cuDevicePrimaryCtxRetain = nullptr;
    PFN_cuDevicePrimaryCtxRelease cuDevicePrimaryCtxRelease = nullptr;
    PFN_cuStreamCreate cuStreamCreate = nullptr;
    PFN_cuStreamDestroy cuStreamDestroy = nullptr;
    PFN_cuStreamAddCallback cuStreamAddCallback = nullptr;
    PFN_cuGraphicsGLRegisterImage cuGraphicsGLRegisterImage = nullptr;
    PFN_cuGraphicsUnregisterResource cuGraphicsUnregisterResource = nullptr;
    PFN_cuGraphicsMapResources cuGraphicsMapResources = nullptr;
    PFN_cuGraphicsUnmapResources cuGraphicsUnmapResources = nullptr;
    PFN_cuGraphicsSubResourceGetMappedArray cuGraphicsSubResourceGetMappedArray = nullptr;
    PFN_cuMemcpy2DAsync cuMemcpy2DAsync = nullptr;
    PFN_cuGetErrorString cuGetErrorString = nullptr;
};

static CudaFuncTable g_cudaFuncs;
static bool g_cudaLibLoaded = false;
static std::mutex g_cudaLoadMutex;

#ifdef Q_OS_WINDOWS
static HMODULE g_cudaLib = nullptr;
#else
static void *g_cudaLib = nullptr;
#endif

static bool loadCudaLibrary()
{
    std::lock_guard<std::mutex> lock(g_cudaLoadMutex);

    if (g_cudaLibLoaded) {
        return true;
    }
#ifdef Q_OS_WINDOWS
    const char dllName[] = "nvcuda.dll";
#else
    const char dllName[] = "libcuda.so";
#endif
    g_cudaLib = loadLibrary(dllName);
    if (g_cudaLib == nullptr) {
        qWarning() << QStringLiteral("Failed to load CUDA library: %1").arg(dllName);
        return false;
    }

// 定义函数加载宏
#define LOAD_CUDA_FUNC(funcName, dllFuncName)                                                  \
    do {                                                                                       \
        g_cudaFuncs.funcName = (PFN_##funcName)GetProcAddress(g_cudaLib, dllFuncName);         \
        if (!g_cudaFuncs.funcName) {                                                           \
            qWarning() << QStringLiteral("Failed to load CUDA function: %1").arg(dllFuncName); \
            failedFunctions.append(dllFuncName);                                               \
        }                                                                                      \
    } while (0)

    QStringList failedFunctions;

    // 加载函数
    LOAD_CUDA_FUNC(cuInit, "cuInit");
    LOAD_CUDA_FUNC(cuCtxSetCurrent, "cuCtxSetCurrent");
    LOAD_CUDA_FUNC(cuCtxGetCurrent, "cuCtxGetCurrent");
    LOAD_CUDA_FUNC(cuDeviceGetCount, "cuDeviceGetCount");
    LOAD_CUDA_FUNC(cuDeviceGet, "cuDeviceGet");
    LOAD_CUDA_FUNC(cuDeviceGetName, "cuDeviceGetName");
    LOAD_CUDA_FUNC(cuDevicePrimaryCtxGetState, "cuDevicePrimaryCtxGetState");
    LOAD_CUDA_FUNC(cuDevicePrimaryCtxSetFlags, "cuDevicePrimaryCtxSetFlags");
    LOAD_CUDA_FUNC(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
    LOAD_CUDA_FUNC(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease");
    LOAD_CUDA_FUNC(cuStreamCreate, "cuStreamCreate");
    LOAD_CUDA_FUNC(cuStreamDestroy, "cuStreamDestroy");
    LOAD_CUDA_FUNC(cuStreamAddCallback, "cuStreamAddCallback");
    LOAD_CUDA_FUNC(cuGraphicsGLRegisterImage, "cuGraphicsGLRegisterImage");
    LOAD_CUDA_FUNC(cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource");
    LOAD_CUDA_FUNC(cuGraphicsMapResources, "cuGraphicsMapResources");
    LOAD_CUDA_FUNC(cuGraphicsUnmapResources, "cuGraphicsUnmapResources");
    LOAD_CUDA_FUNC(cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray");
    LOAD_CUDA_FUNC(cuMemcpy2DAsync, "cuMemcpy2DAsync_v2");
    LOAD_CUDA_FUNC(cuGetErrorString, "cuGetErrorString");

#undef LOAD_CUDA_FUNC

    // 统计加载结果
    if (!failedFunctions.isEmpty()) {
        FreeLibrary(g_cudaLib);
        g_cudaLib = nullptr;
        qWarning() << QStringLiteral("CUDA library loaded failed!");
        return false;
    }

    g_cudaLibLoaded = true;
    qDebug() << QStringLiteral("CUDA library loaded successfully");
    return true;
}

// 检查函数
static bool check(CUresult e, int iLine, const char *szFile)
{
    if (e != CUDA_SUCCESS) {
        const char *errstr = nullptr;
        if (g_cudaFuncs.cuGetErrorString) {
            g_cudaFuncs.cuGetErrorString(e, &errstr);
        }
        qDebug() << QStringLiteral("CUDA error %1 error string: %2 at line %3 in file %4")
                        .arg(e)
                        .arg(errstr ? errstr : "unknown")
                        .arg(iLine)
                        .arg(szFile);
        return false;
    }
    return true;
}

#define ck(call) check(call, __LINE__, __FILE__)

class CudaManager {
public:
    static CudaManager &getInstance()
    {
        static CudaManager instance;
        return instance;
    }

    CUcontext getContext()
    {
        if (!isInitialized()) {
            qWarning() << QStringLiteral("CUDA not initialized!");
            return nullptr;
        }

        int deviceActived = 0;
        unsigned int deviceFlags = 0;
        const unsigned int desiredFlags = CU_CTX_SCHED_BLOCKING_SYNC;

        if (!ck(g_cudaFuncs.cuDevicePrimaryCtxGetState(device_, &deviceFlags, &deviceActived))) {
            qWarning() << QStringLiteral("Failed to get CUDA device state!");
            return nullptr;
        }

        if (deviceActived && deviceFlags != desiredFlags) {
            qWarning() << QStringLiteral(
                "CUDA Primary context already active with incompatible flags!");
        } else if (deviceFlags != desiredFlags) {
            if (!ck(g_cudaFuncs.cuDevicePrimaryCtxSetFlags(device_, desiredFlags))) {
                qWarning() << QStringLiteral("Failed to set CUDA device primary context flags!");
            }
        }

        CUcontext context = nullptr;
        g_cudaFuncs.cuDevicePrimaryCtxRetain(&context, device_);
        return context;
    }

    void releaseContext()
    {
        if (!device_ || !g_cudaLibLoaded)
            return;

        g_cudaFuncs.cuDevicePrimaryCtxRelease(device_);
    }

    CUdevice getDevice()
    {
        return device_;
    }

    bool isInitialized() const
    {
        return isInitialized_.load() && g_cudaLibLoaded;
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

        // 首先尝试加载CUDA库
        if (!loadCudaLibrary()) {
            qWarning() << QStringLiteral("CUDA library not available");
            return;
        }

        if (g_cudaFuncs.cuInit(0) != CUDA_SUCCESS) {
            qWarning() << QStringLiteral("CUDA initialized failed!");
            return;
        }

        int deviceCount = 0;
        if (g_cudaFuncs.cuDeviceGetCount(&deviceCount) != CUDA_SUCCESS) {
            qWarning() << QStringLiteral("CUDA get device count failed!");
            return;
        }

        const auto glRenderer = getCurrentGLRenderer();
        int deviceIndex = -1;
        for (int i = 0; i < deviceCount; ++i) {
            CUdevice dev;
            if (g_cudaFuncs.cuDeviceGet(&dev, i) != CUDA_SUCCESS)
                continue;

            char name[256];
            g_cudaFuncs.cuDeviceGetName(name, 256, dev);
            if (glRenderer.contains(QString::fromStdString(name), Qt::CaseInsensitive)) {
                deviceIndex = i;
                break;
            }
        }

        if (deviceIndex < 0 || deviceIndex >= deviceCount) {
            qInfo()
                << QStringLiteral("Invalid CUDA device index: %1, use 0 instead!").arg(deviceIndex);
            deviceIndex = 0;
        }

        isInitialized_.store(g_cudaFuncs.cuDeviceGet(&device_, deviceIndex) == CUDA_SUCCESS);
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

// 导出CUDA函数供其他模块使用
CUresult cuInit(unsigned int Flags)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuInit(Flags) : CUDA_ERROR_UNKNOWN;
}

CUresult cuCtxSetCurrent(CUcontext ctx)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuCtxSetCurrent(ctx) : CUDA_ERROR_UNKNOWN;
}

CUresult cuCtxGetCurrent(CUcontext *pctx)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuCtxGetCurrent(pctx) : CUDA_ERROR_UNKNOWN;
}

CUresult cuStreamCreate(CUstream *phStream, unsigned int Flags)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuStreamCreate(phStream, Flags) : CUDA_ERROR_UNKNOWN;
}

CUresult cuStreamDestroy(CUstream hStream)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuStreamDestroy(hStream) : CUDA_ERROR_UNKNOWN;
}

CUresult cuStreamAddCallback(CUstream hStream, CUstreamCallback callback, void *userData,
                             unsigned int flags)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuStreamAddCallback(hStream, callback, userData, flags)
                           : CUDA_ERROR_UNKNOWN;
}

CUresult cuGraphicsGLRegisterImage(CUgraphicsResource *pCudaResource, unsigned int image,
                                   unsigned int target, unsigned int Flags)
{
    return g_cudaLibLoaded
               ? g_cudaFuncs.cuGraphicsGLRegisterImage(pCudaResource, image, target, Flags)
               : CUDA_ERROR_UNKNOWN;
}

CUresult cuGraphicsUnregisterResource(CUgraphicsResource resource)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuGraphicsUnregisterResource(resource)
                           : CUDA_ERROR_UNKNOWN;
}

CUresult cuGraphicsMapResources(unsigned int count, CUgraphicsResource *resources, CUstream hStream)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuGraphicsMapResources(count, resources, hStream)
                           : CUDA_ERROR_UNKNOWN;
}

CUresult cuGraphicsUnmapResources(unsigned int count, CUgraphicsResource *resources,
                                  CUstream hStream)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuGraphicsUnmapResources(count, resources, hStream)
                           : CUDA_ERROR_UNKNOWN;
}

CUresult cuGraphicsSubResourceGetMappedArray(CUarray *pArray, CUgraphicsResource resource,
                                             unsigned int arrayIndex, unsigned int mipLevel)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuGraphicsSubResourceGetMappedArray(pArray, resource,
                                                                             arrayIndex, mipLevel)
                           : CUDA_ERROR_UNKNOWN;
}

CUresult cuMemcpy2DAsync(const CUDA_MEMCPY2D *pCopy, CUstream hStream)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuMemcpy2DAsync(pCopy, hStream) : CUDA_ERROR_UNKNOWN;
}

CUresult cuDevicePrimaryCtxRelease(CUdevice dev)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuDevicePrimaryCtxRelease(dev) : CUDA_ERROR_UNKNOWN;
}

CUresult cuGetErrorString(CUresult error, const char **pStr)
{
    return g_cudaLibLoaded ? g_cudaFuncs.cuGetErrorString(error, pStr) : CUDA_ERROR_UNKNOWN;
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
static std::mutex g_loadFuncMtx;

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
    if (!wgl::loadFuncTable()) {
        qWarning() << QStringLiteral("Failed to load WGL function table!");
    }

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

    std::lock_guard l(g_loadFuncMtx);
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

        // 选择创建D3D11设备的显卡
        Microsoft::WRL::ComPtr<IDXGIFactory> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;

        // 创建 DXGI 工厂
        HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory);
        if (FAILED(hr)) {
            qCritical() << QStringLiteral("CreateDXGIFactory failed, HRESULT:") << Qt::hex << hr;
            return;
        }

        const auto glRenderer = getCurrentGLRenderer();
        UINT i = 0;
        while (factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC desc;
            adapter->GetDesc(&desc);
            const auto adapterDesc = QString::fromWCharArray(desc.Description);
            qInfo() << QStringLiteral("D3D11 AdapterDesc: %1").arg(adapterDesc);
            if (glRenderer.contains(adapterDesc, Qt::CaseInsensitive)) {
                qInfo() << QStringLiteral("Find adapter index: %1").arg(QString::number(i));
                break; // 使用OpenGL Context对应的设备
            }

            ++i;
            adapter.Reset();
        }

        hr = D3D11CreateDevice(
            adapter ? adapter.Get() : nullptr, // 如果指定的适配器有效，就是用指定，否则使用默认
            adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, // 硬件驱动
            nullptr,                                                      // 软件驱动句柄
            createDeviceFlags,                                            // 创建标志
            featureLevels,                                                // 特性级别数组
            ARRAYSIZE(featureLevels),                                     // 特性级别数组大小
            D3D11_SDK_VERSION,                                            // SDK版本
            &device_,                                                     // 输出设备
            &featureLevel,                                                // 输出特性级别
            &context                                                      // 输出设备上下文
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

        const auto glRenderer = getCurrentGLRenderer();
        const UINT adapterCount = d3d9ex->GetAdapterCount();
        UINT adapterIndex = D3DADAPTER_DEFAULT;
        for (UINT i = 0; i < adapterCount; ++i) {
            D3DADAPTER_IDENTIFIER9 desc;
            HRESULT hr = d3d9ex->GetAdapterIdentifier(i, 0, &desc);
            if (FAILED(hr)) {
                qWarning()
                    << QStringLiteral("Failed to get adapter identifier for adapter %1").arg(i);
                continue;
            }

            const auto adapterDesc = QString::fromStdString(desc.Description);
            qInfo() << QStringLiteral("D3D9 AdapterDesc: %1").arg(adapterDesc);
            if (glRenderer.contains(adapterDesc, Qt::CaseInsensitive)) {
                adapterIndex = i;
                qInfo()
                    << QStringLiteral("Find adapter index: %1").arg(QString::number(adapterIndex));
                break; // 使用OpenGL Context对应的设备
            }
        }

        D3DDISPLAYMODEEX modeex = {0};
        modeex.Size = sizeof(D3DDISPLAYMODEEX);
        HRESULT hr = d3d9ex->GetAdapterDisplayModeEx(adapterIndex, &modeex, NULL);
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
        hr = d3d9ex->CreateDeviceEx(adapterIndex, D3DDEVTYPE_HAL, NULL,
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
static std::once_flag g_init_flag_;

bool loadFuncTable()
{
    std::call_once(g_init_flag_, []() {
        g_funcTable.egl_create_image_KHR =
            (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        g_funcTable.egl_destroy_image_KHR =
            (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        g_funcTable.gl_egl_image_target_texture2d_oes =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    });

    if (!g_funcTable.egl_create_image_KHR) {
        qCritical() << QStringLiteral("Can not get eglCreateImageKHR proc address!");
        return false;
    }
    if (!g_funcTable.egl_destroy_image_KHR) {
        qCritical() << QStringLiteral("Can not get eglDestroyImageKHR proc address!");
        return false;
    }
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

#ifdef VULKAN_AVAILABLE
#include <memory>

namespace vulkan_utils {

#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#else
#define FORCE_INLINE inline
#endif

/**
 * Count number of bits set to one in x
 * @param x value to count bits of
 * @return the number of bits set to one in x
 */
static FORCE_INLINE int popCount(uint32_t x)
{
    x -= (x >> 1) & 0x55555555;
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    x += x >> 8;
    return (x + (x >> 16)) & 0x3F;
}

#undef FORCE_INLINE

static inline int pickQueueFamily(VkQueueFamilyProperties2 *qf, uint32_t numQf,
                                  VkQueueFlagBits flags)
{
    int index = -1;
    uint32_t minScore = UINT32_MAX;

    for (uint32_t i = 0; i < numQf; i++) {
        VkQueueFlagBits qflags =
            static_cast<VkQueueFlagBits>(qf[i].queueFamilyProperties.queueFlags);

        /* Per the spec, reporting transfer caps is optional for these 2 types */
        if ((flags & VK_QUEUE_TRANSFER_BIT) &&
            (qflags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            qflags =
                static_cast<VkQueueFlagBits>(static_cast<uint32_t>(qflags) | VK_QUEUE_TRANSFER_BIT);

        if (qflags & flags) {
            uint32_t score = popCount(qflags) + qf[i].queueFamilyProperties.timestampValidBits;
            if (score < minScore) {
                index = i;
                minScore = score;
            }
        }
    }

    if (index > -1)
        qf[index].queueFamilyProperties.timestampValidBits++;

    return index;
}

static inline int pickVideoQueueFamily(VkQueueFamilyProperties2 *qf,
                                       VkQueueFamilyVideoPropertiesKHR *qfVid, uint32_t numQf,
                                       VkVideoCodecOperationFlagBitsKHR flags)
{
    int index = -1;
    uint32_t min_score = UINT32_MAX;

    for (uint32_t i = 0; i < numQf; i++) {
        const VkQueueFlagBits qflags =
            static_cast<VkQueueFlagBits>(qf[i].queueFamilyProperties.queueFlags);
        const VkQueueFlagBits vflags = static_cast<VkQueueFlagBits>(qfVid[i].videoCodecOperations);

        if (!(qflags & (VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_VIDEO_DECODE_BIT_KHR)))
            continue;

        if (vflags & flags) {
            uint32_t score = popCount(vflags) + qf[i].queueFamilyProperties.timestampValidBits;
            if (score < min_score) {
                index = i;
                min_score = score;
            }
        }
    }

    if (index > -1)
        qf[index].queueFamilyProperties.timestampValidBits++;

    return index;
}

static int addQueueFamily(std::vector<VkDeviceQueueCreateInfo> &queueCreateInfos,
                          std::vector<std::vector<float>> &priorityStorage,
                          uint32_t queueFamilyIndex, uint32_t queueCount)
{
    if (queueCount == 0) {
        return 0;
    }

    // 已经存在该 queueFamily ?
    for (auto &info : queueCreateInfos) {
        if (info.queueFamilyIndex == queueFamilyIndex) {
            return 0;
        }
    }

    // 为本 queueFamily 创建优先级数组
    priorityStorage.emplace_back(queueCount);
    auto &priorities = priorityStorage.back();

    for (uint32_t i = 0; i < queueCount; ++i) {
        priorities[i] = 1.0f / queueCount;
    }

    // 新建一个 VkDeviceQueueCreateInfo
    VkDeviceQueueCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    info.queueFamilyIndex = queueFamilyIndex;
    info.queueCount = queueCount;
    info.pQueuePriorities = priorities.data(); // 绑定 vector 内的数据

    queueCreateInfos.push_back(info);
    return 0;
}

static const char *toStringMessageSeverity(VkDebugUtilsMessageSeverityFlagBitsEXT s)
{
    switch (s) {
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
            return "VERBOSE";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            return "ERROR";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
            return "WARNING";
        case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
            return "INFO";
        default:
            return "UNKNOWN";
    }
}

static const char *toStringMessageType(VkDebugUtilsMessageTypeFlagsEXT s)
{
    if (s == 7)
        return "General | Validation | Performance";
    if (s == 6)
        return "Validation | Performance";
    if (s == 5)
        return "General | Performance";
    if (s == 4 /*VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT*/)
        return "Performance";
    if (s == 3)
        return "General | Validation";
    if (s == 2 /*VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT*/)
        return "Validation";
    if (s == 1 /*VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT*/)
        return "General";
    return "Unknown";
}

// Default debug messenger
// Feel free to copy-paste it into your own code, change it as needed, then call
// `set_debug_callback()` to use that instead
static VkBool32 customDebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                    VkDebugUtilsMessageTypeFlagsEXT messageType,
                                    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                                    void *)
{
    /* Ignore false positives */
    switch (pCallbackData->messageIdNumber) {
        case 0x086974c1: /* BestPractices-vkCreateCommandPool-command-buffer-reset */
        case 0xfd92477a: /* BestPractices-vkAllocateMemory-small-allocation */
        case 0x618ab1e7: /* VUID-VkImageViewCreateInfo-usage-02275 */
        case 0x30f4ac70: /* VUID-VkImageCreateInfo-pNext-06811 */
            return VK_FALSE;
        default:
            break;
    }

    auto ms = toStringMessageSeverity(messageSeverity);
    auto mt = toStringMessageType(messageType);
    if (messageType & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        qDebug().noquote() << QStringLiteral("[%1: %2] - %3\n%4\n")
                                  .arg(ms, mt, pCallbackData->pMessageIdName,
                                       pCallbackData->pMessage);
    } else {
        qDebug().noquote() << QStringLiteral("[%1: %2]\n%3\n").arg(ms, mt, pCallbackData->pMessage);
    }

    return VK_FALSE; // Applications must return false here (Except Validation, if return true, will
                     // skip calling to driver)
}

class VulkanManager {
public:
    static VulkanManager &getInstance()
    {
        static VulkanManager instance;
        return instance;
    }

    const vkb::InstanceDispatchTable &instanceDispatchTable()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return vkInstanceDispatchTable_;
    }

    const vkb::DispatchTable &dispatchTable()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return vkDispatchTable_;
    }

    const vkb::Instance &instance()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return vkInstance_;
    }

    const vkb::PhysicalDevice &physicalDevice()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return vkPhysicalDevice_;
    }

    const vkb::Device &device()
    {
        std::call_once(initFlag_, [this]() { initialize(); });
        return vkDevice_;
    }

    decoder_sdk::VulkanDeviceContext *deviceContext()
    {
        std::lock_guard<std::mutex> l(contextMtx_);
        std::call_once(initFlag_, [this]() { initialize(); });
        if (!isInitialized())
            return nullptr;

        if (!deviceContext_) {
            setupDeviceContext();
        }

        return deviceContext_.get();
    }

    bool isInitialized() const
    {
        return initialized_;
    }

    void cleanup()
    {
        if (!initialized_)
            return;

        try {
            if (vkDevice_.device != VK_NULL_HANDLE) {
                vkDispatchTable_.fp_vkDeviceWaitIdle(vkDevice_.device);
                vkb::destroy_device(vkDevice_);
            }
        } catch (std::exception &e) {
            qWarning() << QStringLiteral("Destroy VkDevice Failed: %1")
                              .arg(QString::fromStdString(e.what()));
        }

        try {
            if (vkInstance_.instance != VK_NULL_HANDLE) {
                vkb::destroy_instance(vkInstance_);
            }
        } catch (std::exception &e) {
            qWarning() << QStringLiteral("Destroy VkInstance Failed: %1")
                              .arg(QString::fromStdString(e.what()));
        }

        initialized_ = false;
    }

    void lockQueue(uint32_t queue_family, uint32_t index);
    void unlockQueue(uint32_t queue_family, uint32_t index);

    // 禁止拷贝和赋值
    VulkanManager(const VulkanManager &) = delete;
    VulkanManager &operator=(const VulkanManager &) = delete;

private:
    VulkanManager() = default;
    ~VulkanManager()
    {
        cleanup();
    }

    void initialize();

    bool createInstance();
    bool selectPhysicalDevice();
    bool createDevice();

    void setupDeviceContext();

private:
    // 初始化相关
    std::once_flag initFlag_;
    bool initialized_ = false;

    // vulkan实例、物理设备、逻辑设备
    vkb::Instance vkInstance_;
    vkb::PhysicalDevice vkPhysicalDevice_;
    vkb::Device vkDevice_;

    // vulkan函数表
    vkb::InstanceDispatchTable vkInstanceDispatchTable_;
    vkb::DispatchTable vkDispatchTable_;

    // 开启的扩展
    std::vector<const char *> vkDeviceExtensions_;
    std::vector<const char *> vkInstanceExtensions_;

    // 需要的特性
    VkPhysicalDeviceFeatures2 features2_;
    VkPhysicalDeviceVulkan11Features features11_;
    VkPhysicalDeviceVulkan12Features features12_;
    VkPhysicalDeviceVulkan13Features features13_;
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer_;
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT atomicFloat_;
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperativeMatrix_;

    // 队列相关
    int nbqf_ = 0;
    decoder_sdk::VulkanDeviceQueueFamily queueFamily_[64];

    // ffmpeg 上下文所需内容
    std::shared_ptr<decoder_sdk::VulkanDeviceContext> deviceContext_;

    // 队列锁
    std::vector<std::vector<std::unique_ptr<std::mutex>>> queueMtxes_;

    // 获得上下文时的锁
    std::mutex contextMtx_;
};

void VulkanManager::initialize()
{
    try {
        qInfo() << QStringLiteral("Initializing Vulkan Resources...");

        if (!createInstance()) {
            return;
        }
        qInfo() << QStringLiteral("Vulkan instance created");

        if (!selectPhysicalDevice()) {
            vkb::destroy_instance(vkInstance_);
            return;
        }
        qInfo() << QStringLiteral("Physical device selected, device name: %1")
                       .arg(QString::fromStdString(vkPhysicalDevice_.name));

        if (!createDevice()) {
            vkb::destroy_instance(vkInstance_);
            return;
        }
        qInfo() << QStringLiteral("Logical device created");

        initialized_ = true;
    } catch (const std::exception &e) {
        vkb::destroy_device(vkDevice_);
        vkb::destroy_instance(vkInstance_);
        qWarning() << QStringLiteral("Failed to initialize Vulkan: %1").arg(e.what());
    }
}

bool VulkanManager::createInstance()
{
    vkb::InstanceBuilder builder;
    builder = builder
                  .set_app_name(qApp->applicationName().toStdString().c_str())
#ifdef QT_DEBUG
                  .request_validation_layers(true)
#endif
                  .use_default_debug_messenger()
                  .set_debug_callback(customDebugCallback)
                  .set_headless()
                  .require_api_version(1, 3, 0);

    vkInstanceExtensions_ = {
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#ifdef QT_DEBUG
        VK_EXT_LAYER_SETTINGS_EXTENSION_NAME,
#endif
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    const auto systemInfoRet = vkb::SystemInfo::get_system_info();
    if (systemInfoRet.has_value()) {
        const auto &systemInfo = systemInfoRet.value();
        auto iter = vkInstanceExtensions_.cbegin();
        while (iter != vkInstanceExtensions_.cend()) {
            if (!systemInfo.is_extension_available(*iter)) {
                iter = vkInstanceExtensions_.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    for (auto iter = vkInstanceExtensions_.cbegin(); iter != vkInstanceExtensions_.cend(); ++iter) {
        builder.enable_extension(*iter);
    }

    const auto ret = builder.build();

    if (!ret) {
        qWarning() << QStringLiteral("Failed to create Vulkan instance: %1")
                          .arg(QString::fromStdString(ret.error().message()));
        return false;
    }

    vkInstance_ = ret.value();
    vkInstanceDispatchTable_ = vkInstance_.make_table();
    return true;
}

bool VulkanManager::selectPhysicalDevice()
{
    vkb::PhysicalDeviceSelector selector{vkInstance_};
    const auto ret = selector.set_minimum_version(1, 3)
                         .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                         .allow_any_gpu_device_type(true)
                         .add_required_extension(VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME)
                         .add_required_extension(VK_KHR_BIND_MEMORY_2_EXTENSION_NAME)
                         .add_required_extension(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME)
#if defined(Q_OS_WIN)
                         .add_required_extension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)
                         .add_required_extension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)
#elif defined(Q_OS_LINUX)
                         .add_required_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)
                         .add_required_extension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)
#endif
                         .add_required_extension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME)
                         .add_required_extension(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME)
                         .add_required_extension(VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME)
                         .add_required_extension(VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME)
                         .select();

    if (!ret) {
        qWarning() << QStringLiteral("Failed to select Vulkan Physical Device: %1")
                          .arg(QString::fromStdString(ret.error().message()));
        return false;
    }

    vkPhysicalDevice_ = ret.value();

    // 增加可能存在的extensions
    if (!vkPhysicalDevice_.enable_extension_if_present(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME)) {
        qWarning()
            << QStringLiteral("Not supported %1").arg(VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME);
    }
    if (!vkPhysicalDevice_.enable_extension_if_present(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME)) {
        qWarning()
            << QStringLiteral("Not supported %1").arg(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
    }

    return true;
}

bool VulkanManager::createDevice()
{
    vkb::DeviceBuilder device_builder{vkPhysicalDevice_};

    // ================================
    // 开始配置 VkPhysicalDeviceFeatures2 链
    // ================================
    features2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features11_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features12_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features13_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    descriptorBuffer_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    atomicFloat_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT;
    cooperativeMatrix_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;

    // 手动链 pNext
    features2_.pNext = &features11_;
    features11_.pNext = &features12_;
    features12_.pNext = &features13_;
    features13_.pNext = &descriptorBuffer_;
    descriptorBuffer_.pNext = &atomicFloat_;
    atomicFloat_.pNext = &cooperativeMatrix_;
    cooperativeMatrix_.pNext = nullptr;

    // 调用 Vulkan 原生接口获取所有支持的特性
    if (!vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceFeatures2) {
        qWarning() << QStringLiteral("vkGetPhysicalDeviceFeatures2 not supported!");
        return false;
    }
    vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceFeatures2(vkPhysicalDevice_.physical_device,
                                                             &features2_);

    // ================================
    // 开始配置 队列
    // ================================
    // Advanced: Get the VkQueueFamilyProperties of the device if special queue setup is needed
    std::vector<VkQueueFamilyProperties2> qf;
    std::vector<VkQueueFamilyVideoPropertiesKHR> qfProp;
    uint32_t num = 0;

    // 调用 Vulkan 原生接口获取所有队列属性
    if (!vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceQueueFamilyProperties) {
        qWarning() << QStringLiteral("vkGetPhysicalDeviceQueueFamilyProperties not supported!");
        return false;
    }
    vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceQueueFamilyProperties(
        vkPhysicalDevice_.physical_device, &num, NULL);
    if (!num) {
        qWarning() << QStringLiteral("Failed to get queue family properties!");
        return false;
    }

    // 构造队列列表
    qf.resize(num);
    qfProp.resize(num);

    for (uint32_t i = 0; i < num; i++) {
        qfProp[i] = VkQueueFamilyVideoPropertiesKHR();
        qfProp[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;

        qf[i] = VkQueueFamilyProperties2();
        qf[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
        qf[i].pNext = &qfProp[i];
    }

    // 检索队列族
    if (!vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceQueueFamilyProperties2) {
        qWarning() << QStringLiteral("vkGetPhysicalDeviceQueueFamilyProperties2 not supported!");
        return false;
    }
    vkInstanceDispatchTable_.fp_vkGetPhysicalDeviceQueueFamilyProperties2(
        vkPhysicalDevice_.physical_device, &num, qf.data());
    for (auto &q : qf) {
        //// debug info
        // qDebug() << QStringLiteral("Queue family ") << q.queueFamilyProperties.queueCount
        //          << QStringLiteral(" flags: ") << q.queueFamilyProperties.queueFlags;

        q.queueFamilyProperties.timestampValidBits = 0;
    }

    // 找到所用的队列
#define PICK_QF(type, vidOp)                                                                \
    do {                                                                                    \
        int i;                                                                              \
        int idx;                                                                            \
                                                                                            \
        if (vidOp)                                                                          \
            idx = pickVideoQueueFamily(qf.data(), qfProp.data(), num, vidOp);               \
        else                                                                                \
            idx = pickQueueFamily(qf.data(), num, type);                                    \
                                                                                            \
        if (idx == -1)                                                                      \
            continue;                                                                       \
                                                                                            \
        for (i = 0; i < nbqf_; i++) {                                                       \
            if (queueFamily_[i].idx == idx) {                                               \
                queueFamily_[i].flags = static_cast<VkQueueFlagBits>(                       \
                    static_cast<uint32_t>(queueFamily_[i].flags) | type);                   \
                queueFamily_[i].video_caps = static_cast<VkVideoCodecOperationFlagBitsKHR>( \
                    static_cast<uint32_t>(queueFamily_[i].video_caps) | vidOp);             \
                break;                                                                      \
            }                                                                               \
        }                                                                                   \
        if (i == nbqf_) {                                                                   \
            queueFamily_[i].idx = idx;                                                      \
            queueFamily_[i].num = qf[idx].queueFamilyProperties.queueCount;                 \
            queueFamily_[i].flags = type;                                                   \
            queueFamily_[i].video_caps = vidOp;                                             \
            nbqf_++;                                                                        \
        }                                                                                   \
    } while (0)

    PICK_QF(VK_QUEUE_GRAPHICS_BIT, VK_VIDEO_CODEC_OPERATION_NONE_KHR);
    PICK_QF(VK_QUEUE_COMPUTE_BIT, VK_VIDEO_CODEC_OPERATION_NONE_KHR);
    PICK_QF(VK_QUEUE_TRANSFER_BIT, VK_VIDEO_CODEC_OPERATION_NONE_KHR);

    PICK_QF(VK_QUEUE_VIDEO_DECODE_BIT_KHR, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR);
    PICK_QF(VK_QUEUE_VIDEO_DECODE_BIT_KHR, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR);
    PICK_QF(VK_QUEUE_VIDEO_DECODE_BIT_KHR, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR);

#undef PICK_QF

    // 并且在这创建队列锁
    std::vector<vkb::CustomQueueDescription> queue_descriptions;
    for (uint32_t i = 0; i < num; ++i) {
        if (queueFamily_[i].num <= 0 || queueFamily_[i].idx < 0)
            continue;

        queue_descriptions.push_back(vkb::CustomQueueDescription{
            static_cast<uint32_t>(queueFamily_[i].idx),
            std::vector<float>(queueFamily_[i].num, 1.0f / queueFamily_[i].num)});

        // 为每个队列创建一个锁
        const size_t family_idx = static_cast<size_t>(queueFamily_[i].idx);
        const size_t queue_count = static_cast<size_t>(queueFamily_[i].num);
        if (queueMtxes_.size() <= family_idx) {
            queueMtxes_.resize(family_idx + 1);
        }
        auto &locks = queueMtxes_[family_idx];
        if (locks.size() < queue_count) {
            locks.resize(queue_count);
        }
        for (size_t j = 0; j < queue_count; ++j) {
            if (!locks[j]) {
                locks[j] = std::make_unique<std::mutex>();
            }
        }
    }

    auto ret = device_builder.add_pNext(&features2_)
                   .add_pNext(&features11_)
                   .add_pNext(&features12_)
                   .add_pNext(&features13_)
                   .add_pNext(&descriptorBuffer_)
                   .add_pNext(&atomicFloat_)
                   .add_pNext(&cooperativeMatrix_)
                   .custom_queue_setup(queue_descriptions)
                   .build();

    if (!ret) {
        qWarning() << QStringLiteral("Failed to create Vulkan device: ")
                          .arg(QString::fromStdString(ret.error().message()));
        return false;
    }

    vkDevice_ = ret.value();
    vkDispatchTable_ = vkDevice_.make_table();
    return true;
}

void VulkanManager::setupDeviceContext()
{
    if (deviceContext_)
        return;

    deviceContext_ = std::make_shared<decoder_sdk::VulkanDeviceContext>();
    deviceContext_->get_proc_addr = vkInstance_.fp_vkGetInstanceProcAddr;

    // 设置基本的Vulkan对象
    deviceContext_->inst = vkInstance_.instance;
    deviceContext_->phys_dev = vkPhysicalDevice_.physical_device;
    deviceContext_->act_dev = vkDevice_.device;

    // 获得设备特性结构体
    deviceContext_->device_features = features2_;

    // 获取实例扩展信息
    deviceContext_->enabled_inst_extensions = vkInstanceExtensions_.data();
    deviceContext_->nb_enabled_inst_extensions = static_cast<int>(vkInstanceExtensions_.size());

    // 获取设备扩展信息
    static auto deviceExts = vkPhysicalDevice_.get_extensions();
    vkDeviceExtensions_.clear();
    vkDeviceExtensions_.reserve(deviceExts.size());
    for (const auto &ext : deviceExts) {
        vkDeviceExtensions_.push_back(ext.c_str());
    }
    deviceContext_->enabled_dev_extensions =
        vkDeviceExtensions_.empty() ? nullptr : vkDeviceExtensions_.data();
    deviceContext_->nb_enabled_dev_extensions = static_cast<int>(vkDeviceExtensions_.size());

    deviceContext_->nb_qf = nbqf_;
    for (int i = 0; i < nbqf_; ++i) {
        deviceContext_->qf[i] = queueFamily_[i];
    }

    // 设置队列锁回调
    deviceContext_->lock_queue = [](struct AVHWDeviceContext *ctx, uint32_t queue_family,
                                    uint32_t index) {
        VulkanManager::getInstance().lockQueue(queue_family, index);
    };
    deviceContext_->unlock_queue = [](struct AVHWDeviceContext *ctx, uint32_t queue_family,
                                      uint32_t index) {
        VulkanManager::getInstance().unlockQueue(queue_family, index);
    };
}

void VulkanManager::lockQueue(uint32_t queue_family, uint32_t index)
{
    if (queueMtxes_.size() <= queue_family)
        return;
    auto &locks = queueMtxes_[queue_family];
    if (locks.size() <= index || !locks[index])
        return;
    locks[index]->lock();
}

void VulkanManager::unlockQueue(uint32_t queue_family, uint32_t index)
{
    if (queueMtxes_.size() <= queue_family)
        return;
    auto &locks = queueMtxes_[queue_family];
    if (locks.size() <= index || !locks[index])
        return;
    locks[index]->unlock();
}

const vkb::InstanceDispatchTable &getInstanceDispatchTable()
{
    return VulkanManager::getInstance().instanceDispatchTable();
}

const vkb::DispatchTable &getDispatchTable()
{
    return VulkanManager::getInstance().dispatchTable();
}

const vkb::Instance &getVkInstance()
{
    return VulkanManager::getInstance().instance();
}

const vkb::PhysicalDevice &getVkPhysicalDevice()
{
    return VulkanManager::getInstance().physicalDevice();
}

const vkb::Device &getVkDevice()
{
    return VulkanManager::getInstance().device();
}

bool isVulkanAvaliable()
{
    return VulkanManager::getInstance().isInitialized();
}

decoder_sdk::VulkanDeviceContext *getDeviceContext()
{
    return VulkanManager::getInstance().deviceContext();
}

void shutdown()
{
    return VulkanManager::getInstance().cleanup();
}
} // namespace vulkan_utils
#endif
