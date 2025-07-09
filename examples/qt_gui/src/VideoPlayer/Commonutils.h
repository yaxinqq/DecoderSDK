#pragma once
void registerVideoMetaType();

#ifdef CUDA_AVAILABLE
#include <cuda.h>
namespace cuda_utils {
CUcontext getCudaContext();
CUdevice getCudaDevice();
bool isCudaAvailable();
} // namespace cuda_utils
#endif

#ifdef D3D11VA_AVAILABLE
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
namespace d3d11_utils {
Microsoft::WRL::ComPtr<ID3D11Device> getD3D11Device();
bool isD3D11Available();
} // namespace d3d11_utils
#endif

#ifdef DXVA2_AVAILABLE
#include <d3d9.h>
#include <dxva2api.h>
#include <wrl/client.h>
namespace dxva2_utils {
Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> getDXVA2DeviceManager();
Microsoft::WRL::ComPtr<IDirect3DDevice9Ex> getDXVA2Device();
bool isDXVA2Available();
} // namespace dxva2_utils
#endif

#ifdef VAAPI_AVAILABLE
#ifdef __unix
#include <GL/gl.h>
#endif

#include "decodersdk/vaapi_utils.h"

namespace egl {
typedef void* EGLImageKHR;
typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLClientBuffer;
typedef unsigned int EGLenum;
typedef unsigned int EGLBoolean;
typedef int32_t EGLint;

bool loadFuncTable();

EGLImageKHR egl_create_image_KHR(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
EGLBoolean egl_destroy_image_KHR(EGLDisplay dpy, EGLImageKHR image);
void gl_egl_image_target_texture2d_oes(GLenum target, GLeglImageOES image);
}

namespace vaapi_utils {    
VADisplay getVADisplayDRM();
bool isVAAPIAvailable();
}
#endif