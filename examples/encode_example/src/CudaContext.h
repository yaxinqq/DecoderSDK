#pragma once

#include <cuda.h>

/**
 * @brief CUDA上下文的封装
 * 
 * @param 
 * @return
 */
class CudaContext {
public:
    CudaContext();
    ~CudaContext();

    CudaContext(const CudaContext &) = delete;
    CudaContext &operator=(const CudaContext &) = delete;

    /**
     * @brief 根据给定的设备索引，进行初始化
     * 
     * @param deviceIndex 设备索引
     * @return
     */
    bool initialize(int deviceIndex);
    /**
     * @brief 安全退出
     */
    void shutdown();

    /**
     * @brief 返回CUDA上下文
     *
     * @return 上下文
     */
    CUcontext context() const { return ctx_; }

private:
    CUdevice device_ = 0;
    CUcontext ctx_ = nullptr;
};

