#include <iostream>
#include <chrono>
#include <iomanip>
#include "DecoderManager.h"

extern "C" {
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
}

// 用于计算帧率的辅助函数
class FPSCalculator {
private:
    std::chrono::steady_clock::time_point startTime;
    int frameCount;
    double fps;
    
public:
    FPSCalculator() : frameCount(0), fps(0.0) {
        startTime = std::chrono::steady_clock::now();
    }
    
    void update() {
        frameCount++;
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
        
        // 每秒更新一次FPS
        if (duration >= 1000) {
            fps = frameCount * 1000.0 / duration;
            frameCount = 0;
            startTime = now;
        }
    }
    
    double getFPS() const {
        return fps;
    }
};

int main(int argc, char* argv[]) {
    avdevice_register_all();
    avformat_network_init();

    std::cout << "开始解码测试..." << std::endl;
    
    // 允许从命令行指定视频文件
    std::string videoPath = "C:\\Users\\win10\\Desktop\\test_video\\test.mp4";
    if (argc > 1) {
        videoPath = argv[1];
    }
    
    std::cout << "测试文件: " << videoPath << std::endl;

    // 创建解码管理器
    DecoderManager manager;
    auto startTime = std::chrono::steady_clock::now();
    
    if (!manager.open(videoPath)) {
        std::cerr << "打开文件失败: " << videoPath << std::endl;
        return -1;
    }
    
    auto openDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    std::cout << "文件打开成功，耗时: " << openDuration << "ms" << std::endl;
    
    // 设置音频作为主时钟（默认）
    manager.setMasterClock(SyncController::MasterClock::Audio);
    
    // 启用帧率控制（按照实际帧率推送视频帧）
    manager.setFrameRateControl(true);
    
    // 开始解码
    std::cout << "开始解码..." << std::endl;
    manager.start();
    
    // 获取视频帧率
    double videoFrameRate = manager.getVideoFrameRate();
    std::cout << "检测到的视频帧率: " << videoFrameRate << " fps" << std::endl;
    
    // 统计变量
    int64_t lastVideoPts = AV_NOPTS_VALUE;
    int64_t lastAudioPts = AV_NOPTS_VALUE;
    int frameCount = 0;
    int videoFrameCount = 0;
    int audioFrameCount = 0;
    int discontinuityCount = 0;
    
    // 帧率计算
    FPSCalculator videoFPS;
    FPSCalculator audioFPS;
    
    // 设置最大帧数限制或测试时长
    const int MAX_FRAMES = 10000;  // 可以根据需要调整
    const int TEST_DURATION_SEC = 30;  // 测试30秒
    
    auto testStartTime = std::chrono::steady_clock::now();
    bool testRunning = true;
    
    std::cout << std::fixed << std::setprecision(2);
    
    // 主测试循环
    while (testRunning && frameCount < MAX_FRAMES) {
        // 检查是否达到测试时长
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
            currentTime - testStartTime).count();
        if (elapsedSec >= TEST_DURATION_SEC) {
            std::cout << "达到测试时长限制: " << TEST_DURATION_SEC << "秒" << std::endl;
            testRunning = false;
            break;
        }
        
        // 获取视频帧
        Frame videoFrame;
        if (manager.videoQueue().popFrame(videoFrame, 5)) {
            videoFPS.update();
            
            // 检查视频帧的连续性
            AVFrame* frame = videoFrame.get();
            if (lastVideoPts != AV_NOPTS_VALUE && frame->pts <= lastVideoPts) {
                std::cout << "视频帧时间戳异常: 当前=" << frame->pts 
                         << " 上一帧=" << lastVideoPts << std::endl;
                discontinuityCount++;
            }
            lastVideoPts = frame->pts;

            // 每100帧输出一次视频帧信息
            if (videoFrameCount % 100 == 0) {
                std::cout << "视频帧 #" << videoFrameCount << ": " 
                          << frame->width << "x" << frame->height
                          << " 格式=" << frame->format 
                          << " PTS=" << frame->pts 
                          << " 类型=" << av_get_picture_type_char(frame->pict_type) 
                          << " 硬件解码=" << (videoFrame.isInHardware() ? "是" : "否")
                          << std::endl;
            }

            videoFrameCount++;
        }

        // 获取音频帧
        Frame audioFrame;
        if (manager.audioQueue().popFrame(audioFrame, 5)) {
            audioFPS.update();
            
            // 检查音频帧的连续性
            AVFrame* frame = audioFrame.get();
            if (lastAudioPts != AV_NOPTS_VALUE && frame->pts <= lastAudioPts) {
                std::cout << "音频帧时间戳异常: 当前=" << frame->pts 
                         << " 上一帧=" << lastAudioPts << std::endl;
                discontinuityCount++;
            }
            lastAudioPts = frame->pts;

            // 每500帧输出一次音频帧信息
            if (audioFrameCount % 500 == 0) {
                std::cout << "音频帧 #" << audioFrameCount << ": "
                          << "采样数=" << frame->nb_samples
                          << " 采样率=" << frame->sample_rate
                          << " 格式=" << frame->format
                          << " PTS=" << frame->pts << std::endl;
            }

            audioFrameCount++;
        }

        // 如果没有获取到任何帧，短暂休眠避免CPU占用过高
        if (!videoFrame.get() && !audioFrame.get()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        frameCount++;
        
        // 每秒输出一次统计信息
        if (frameCount % 100 == 0) {
            std::cout << "解码状态 [" << elapsedSec << "s]: "
                      << "视频=" << videoFrameCount << " (" << videoFPS.getFPS() << " fps), "
                      << "音频=" << audioFrameCount << " 帧, "
                      << "不连续=" << discontinuityCount << std::endl;
        }
    }

    auto testEndTime = std::chrono::steady_clock::now();
    auto totalDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        testEndTime - testStartTime).count();
    
    // 输出最终统计信息
    std::cout << "\n解码测试完成" << std::endl;
    std::cout << "总时长: " << totalDuration / 1000.0 << " 秒" << std::endl;
    std::cout << "视频帧: " << videoFrameCount 
              << " (平均 " << (videoFrameCount * 1000.0 / totalDuration) << " fps)" << std::endl;
    std::cout << "音频帧: " << audioFrameCount 
              << " (平均 " << (audioFrameCount * 1000.0 / totalDuration) << " fps)" << std::endl;
    std::cout << "时间戳不连续: " << discontinuityCount << " 次" << std::endl;
    
    // 停止解码
    manager.stop();
    avformat_network_deinit();
    
    return 0;
}