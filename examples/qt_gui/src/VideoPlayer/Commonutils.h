#pragma once
void registerVideoMetaType();

#ifdef CUDA_AVAILABLE
#include <cuda.h>
namespace CudaUtils {
CUcontext getCudaContext();
bool isCudaAvailable();
} // namespace CudaUtils
#endif

#ifdef D3D11VA_AVAILABLE
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
namespace D3D11Utils {
Microsoft::WRL::ComPtr<ID3D11Device> getD3D11Device();
bool isD3D11Available();
} // namespace D3D11Utils
#endif

#ifdef DXVA2_AVAILABLE
#include <d3d9.h>
#include <dxva2api.h>
#include <wrl/client.h>
namespace DXVA2Utils {
Microsoft::WRL::ComPtr<IDirect3DDeviceManager9> getDXVA2DeviceManager();
bool isDXVA2Available();
} // namespace DXVA2Utils
#endif