#pragma once
void registerVideoMetaType();

#ifdef CUDA_AVAILABLE
#include <cuda.h>
namespace CudaUtils {
CUcontext getCudaContext();
bool isCudaAvailable();
} // namespace CudaUtils
#endif