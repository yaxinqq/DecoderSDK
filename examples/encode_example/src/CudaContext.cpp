#include "CudaContext.h"

CudaContext::CudaContext() = default;

CudaContext::~CudaContext()
{
    shutdown();
}

bool CudaContext::initialize(int deviceIndex)
{
    if (ctx_) {
        return true;
    }

    // 初始化cuda
    if (cuInit(0) != CUDA_SUCCESS) {
        return false;
    }

    // 找到对应的设备
    if (cuDeviceGet(&device_, deviceIndex) != CUDA_SUCCESS) {
        return false;
    }

    // 获得上下文
    if (cuDevicePrimaryCtxRetain(&ctx_, device_) != CUDA_SUCCESS) {
        ctx_ = nullptr;
        return false;
    }

    // 设为当前
    if (cuCtxSetCurrent(ctx_) != CUDA_SUCCESS) {
        shutdown();
        return false;
    }

    return true;
}

void CudaContext::shutdown()
{
    if (!ctx_) {
        return;
    }

    // 释放上下文
    cuDevicePrimaryCtxRelease(device_);
    ctx_ = nullptr;
}