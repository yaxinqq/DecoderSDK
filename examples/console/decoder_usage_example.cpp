/**
 * @file decoder_usage_example.cpp
 * @brief DecoderSDK 完整编解码流程示例
 *
 * 本示例演示了如何使用 DecoderSDK 进行视频流的解码、处理（转发）和重新编码。
 * 包含以下主要步骤：
 * 1. 初始化 DecoderController 打开输入流（文件或RTSP）。
 * 2. 初始化 EncoderController 打开输出文件。
 * 3. 将解码得到的音视频帧推送到编码器进行编码。
 * 4. 演示了多线程处理和基本的同步机制。
 *
 *
 * 运行参数：
 * ./decoder_usage_example [input_url] [output_file]
 */

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "decodersdk/common_define.h"
#include "decodersdk/decoder_controller.h"
#include "decodersdk/encoder_controller.h"
#include "logger/logger.h"

using namespace decoder_sdk;

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    // 1. 解析参数
    std::string inputPath = (argc > 1) ? argv[1] : "D:/WorkSpace/test_video/test.mp4";
    std::string outputPath = (argc > 2) ? argv[2] : "output.mp4";

    std::cout << "Input: " << inputPath << std::endl;
    std::cout << "Output: " << outputPath << std::endl;

    // 2. 初始化解码器
    DecoderController decoder;
    DecoderConfig decodeConfig;
    decodeConfig.hwAccelType = HWAccelType::kAuto; // 自动选择硬解

    if (!decoder.open(inputPath, decodeConfig)) {
        std::cerr << "Failed to open decoder: " << inputPath << std::endl;
        return -1;
    }
    std::cout << "Decoder opened successfully." << std::endl;

    // 获取流信息以配置编码器
    const auto &streamInfoOpt = decoder.streamInfo();
    if (!streamInfoOpt) {
        std::cerr << "Failed to get stream info." << std::endl;
        return -1;
    }
    const StreamInfo &streamInfo = *streamInfoOpt;

    // 3. 初始化编码器
    EncoderController encoder;
    EncoderConfig encodeConfig;
    encodeConfig.url = outputPath;
    encodeConfig.format = "mp4";
    encodeConfig.hwAccelType = HWAccelType::kAuto;
    encodeConfig.encodeFormat = ImageFormat::kNV12; // 和解码端保持一致

    // 配置视频参数
    if (streamInfo.videoInfo) {
        encodeConfig.encodeMediaTypes.set(MediaType::kVideo);
        encodeConfig.width = streamInfo.videoInfo->width;
        encodeConfig.height = streamInfo.videoInfo->height;
        encodeConfig.fps =
            streamInfo.videoInfo->frameRate > 0 ? streamInfo.videoInfo->frameRate : 30;
        encodeConfig.videoBitrate = 4000000; // 4Mbps
        std::cout << "Video Config: " << encodeConfig.width << "x" << encodeConfig.height << " @"
                  << encodeConfig.fps << "fps" << std::endl;
    } else {
        encodeConfig.encodeMediaTypes.set(MediaType::kVideo, false);
    }

    // 配置音频参数
    if (streamInfo.audioInfo) {
        encodeConfig.encodeMediaTypes.set(MediaType::kAudio);
        encodeConfig.sampleRate = streamInfo.audioInfo->sampleRate;
        encodeConfig.channels = streamInfo.audioInfo->channels;
        encodeConfig.audioBitrate = 128000; // 128kbps
        std::cout << "Audio Config: " << encodeConfig.sampleRate << "Hz " << encodeConfig.channels
                  << "ch" << std::endl;
    } else {
        encodeConfig.encodeMediaTypes.set(MediaType::kAudio, false);
    }

    if (!encoder.open(outputPath, encodeConfig)) {
        std::cerr << "Failed to open encoder: " << outputPath << std::endl;
        return -1;
    }
    std::cout << "Encoder opened successfully." << std::endl;

    // 4. 启动任务
    encoder.start();
    decoder.startDecode();

    std::atomic<bool> running{true};

    // 音频处理线程
    std::thread audioThread([&]() {
        Frame frame;
        while (running) {
            // 从解码器获取帧
            auto audioQ = decoder.audioQueue();

            if (audioQ.pop(frame, 10)) {
                if (frame.isValid()) {
                    // 推送到编码器
                    if (!encoder.pushFrame(MediaType::kAudio, frame)) {
                        // std::cerr << "Failed to push audio frame" << std::endl;
                    }
                }
            } else {
                // Wait/Sleep
            }
        }
    });

    // 视频处理线程
    std::thread videoThread([&]() {
        Frame frame;
        int frameCount = 0;
        while (running) {
            auto videoQ = decoder.videoQueue();

            if (videoQ.pop(frame, 10)) {
                if (frame.isValid()) {
                    // 推送到编码器
                    if (!encoder.pushFrame(MediaType::kVideo, frame)) {
                        // std::cerr << "Failed to push video frame" << std::endl;
                    }

                    if (++frameCount % 30 == 0) {
                        std::cout << "Processed " << frameCount
                                  << " video frames. PTS: " << frame.secPts() << "\r" << std::flush;
                    }
                }
            } else {
                // Wait/Sleep
            }
        }
    });

    // 主循环：运行一段时间
    int seconds = 20;
    std::cout << "Running for " << seconds << " seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(seconds));

    // 5. 停止和清理
    std::cout << "\nStopping..." << std::endl;
    running = false;

    if (audioThread.joinable())
        audioThread.join();
    if (videoThread.joinable())
        videoThread.join();

    decoder.stopDecode();
    decoder.close();

    encoder.stop();
    encoder.close();

    std::cout << "Done." << std::endl;
    return 0;
}
