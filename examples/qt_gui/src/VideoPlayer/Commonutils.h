#pragma once
#include <QString>

void registerVideoMetaType();
void clearGPUResource();
QString getCurrentGLRenderer();

#ifdef CUDA_AVAILABLE
#include <cuda.h>
namespace cuda_utils {
CUcontext getCudaContext();
void releaseContext();
CUdevice getCudaDevice();
bool isCudaAvailable();
} // namespace cuda_utils
#endif

#if defined(D3D11VA_AVAILABLE) || defined(DXVA2_AVAILABLE)
#include <Windows.h>
#include <wrl/client.h>
#include <GL/gl.h>
#include <atomic>
namespace wgl {
// WGL-DX interop definitions
#define WGL_ACCESS_READ_ONLY_NV 0x00000000
#define WGL_ACCESS_READ_WRITE_NV 0x00000001
#define WGL_ACCESS_WRITE_DISCARD_NV 0x00000002

bool loadFuncTable();

class WglDeviceRef {
public:
    WglDeviceRef() = default;

    // 从D3D11、D3D9设备创建WGL设备
    explicit WglDeviceRef(void *dxObject);

    WglDeviceRef(const WglDeviceRef &other) noexcept;
    WglDeviceRef &operator=(const WglDeviceRef &other) noexcept;
    WglDeviceRef(WglDeviceRef &&other) noexcept;
    WglDeviceRef &operator=(WglDeviceRef &&other) noexcept;
    ~WglDeviceRef();

    HANDLE get() const noexcept;
    void reset(HANDLE new_handle = nullptr);
    int use_count() const noexcept;
    bool isValid() const noexcept;
    explicit operator bool() const noexcept;
    bool operator==(const WglDeviceRef &other) const noexcept;
    bool operator!=(const WglDeviceRef &other) const noexcept;

    // wgl互操作函数
    HANDLE wglDXRegisterObjectNV(void *dxObject, GLuint name, GLenum type, GLenum access);
    BOOL wglDXUnregisterObjectNV(HANDLE hObject);
    BOOL wglDXLockObjectsNV(GLint count, HANDLE *hObjects);
    BOOL wglDXUnlockObjectsNV(GLint count, HANDLE *hObjects);

private:
    struct ControlBlock {
        std::atomic<int> refCount{1};
        HANDLE wglHandle = nullptr;

        explicit ControlBlock(HANDLE handle);
        ~ControlBlock();
    };

    void acquire(ControlBlock *ctrl) noexcept;
    void release() noexcept;
    HANDLE createWglDevice(void *device);

    ControlBlock *control_ = nullptr;
};

// wgl设置共享资源
BOOL wglDXSetResourceShareHandleNV(void *dxObject, HANDLE shareHandle);
} // namespace wgl
#endif

#ifdef D3D11VA_AVAILABLE
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
namespace d3d11_utils {
Microsoft::WRL::ComPtr<ID3D11Device> getD3D11Device();
wgl::WglDeviceRef getWglDeviceRef();
bool isD3D11Available();
void shutdown();
} // namespace d3d11_utils
#endif

#ifdef DXVA2_AVAILABLE
#include <d3d9.h>
#include <dxva2api.h>
#include <wrl/client.h>
namespace dxva2_utils {
Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> getDXVA2DeviceManager();
Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> getDXVA2Device();
wgl::WglDeviceRef getWglDeviceRef();
bool isDXVA2Available();
void shutdown();
} // namespace dxva2_utils
#endif

#ifdef VAAPI_AVAILABLE
#ifdef __unix
#include <GL/gl.h>
#endif

#include "decodersdk/vaapi_utils.h"

namespace egl {
typedef void *EGLImageKHR;
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLClientBuffer;
typedef unsigned int EGLenum;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;

bool loadFuncTable();

EGLImageKHR egl_create_image_KHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                 EGLClientBuffer buffer, const EGLint *attrib_list);
EGLBoolean egl_destroy_image_KHR(EGLDisplay dpy, EGLImageKHR image);
void gl_egl_image_target_texture2d_oes(GLenum target, GLeglImageOES image);
} // namespace egl

namespace vaapi_utils {
VADisplay getVADisplayDRM();
bool isVAAPIAvailable();
} // namespace vaapi_utils
#endif