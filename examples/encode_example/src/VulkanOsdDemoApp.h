#pragma once

#include "VideoDecoder.h"
#include "VulkanRenderer.h"

#include <string>

class VulkanOsdDemoApp {
public:
    VulkanOsdDemoApp();
    ~VulkanOsdDemoApp();

    /**
     * @brief 运行Demo。
     * @param inputPath 支持文件路径 / rtsp / http 等 DecoderSDK 可打开的地址。
     * @param outPath 输出文件路径
     * @param debug 是否开启调试模式
     * @return 进程退出码。
     */
    int run(const std::string &inputPath, const std::string &outPath, bool debug = true);

private:
    /**
     * @brief 格式化当前本地时间为 UTF-8 字符串。
     * @return 形如 "2026年02月28日 16:20:30" 的 UTF-8 字符串。
     */
    static std::string formatCurrentTimestampUtf8();

private:
    VideoDecoder decoder_;
    VulkanRenderer renderer_;
};

