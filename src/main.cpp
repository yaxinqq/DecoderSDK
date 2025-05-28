#include <chrono>
#include <iomanip>
#include <thread>

extern "C" {
#include <libavutil/time.h>
#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
}

#include "DecoderController.h"
#include "Logger.h"
#include "Utils.h"

// 用于计算帧率的辅助函数
class FPSCalculator {
   private:
    std::chrono::steady_clock::time_point startTime;
    int frameCount;
    double fps;

   public:
    FPSCalculator() : frameCount(0), fps(0.0)
    {
        startTime = std::chrono::steady_clock::now();
    }

    void update()
    {
        frameCount++;
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - startTime)
                            .count();

        // 每秒更新一次FPS
        if (duration >= 1000) {
            fps = frameCount * 1000.0 / duration;
            frameCount = 0;
            startTime = now;
        }
    }

    double getFPS() const
    {
        return fps;
    }
};

int main(int argc, char* argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    avdevice_register_all();
    avformat_network_init();

    // 初始化日志
    Logger::initFromConfig("./etc/decodersdk.json");

    LOG_INFO("开始解码测试...");
    std::string videoPath =
        (argc > 1) ? argv[1] : "C:/Users/win10/Desktop/test_video/test.mp4";

    // std::string videoPath = (argc > 1) ? argv[1]
    //                                    :
    //                                    "rtsp://admin:zhkj2501@192.168.0.71:554/ch1/stream1";

    float playbackSpeed = 3.0f;
    DecoderController manager;
    if (!manager.open(videoPath)) {
        LOG_ERROR("打开文件失败: {}", videoPath);
        return -1;
    }
    LOG_INFO("打开文件成功: {}", videoPath);
    manager.setFrameRateControl(true);  // 关闭内部帧率控制
    manager.setSpeed(playbackSpeed);
    manager.startDecode();

    // 测试时长和计时
    const int TEST_DURATION_SEC = 10;
    auto testStart = std::chrono::steady_clock::now();
    std::atomic<bool> running{true};

    // FPS 统计
    FPSCalculator audioFPS, videoFPS;
    std::atomic<int> audioCount{0}, videoCount{0};

    // 启动音频线程
    double lastAudioPts;
    std::thread audioThread([&]() {
        while (running) {
            Frame afr;
            if (manager.audioQueue().popFrame(afr, 1)) {
                double audioPts = afr.pts();
                LOG_DEBUG("音频帧PTS: {:.2f}", audioPts);
                lastAudioPts = audioPts;
                audioFPS.update();
                audioCount++;
            } else {
                // utils::highPrecisionSleep(1); // 1ms
            }
        }
    });

    // 启动视频线程
    double lastVideoPts;
    std::thread videoThread([&]() {
        while (running) {
            Frame vfr;
            if (manager.videoQueue().popFrame(vfr, 1)) {
                double videoPts = vfr.pts();
                LOG_DEBUG("视频帧PTS: {:.2f}", videoPts);
                lastVideoPts = videoPts;
                videoFPS.update();
                videoCount++;

                // 当视频到达100帧后，调用seek
                // if (videoCount.load() % 100 == 0) {
                //    double seekPos = videoPts + 3.0; // 5秒后
                //    manager.seek(seekPos);
                //    LOG_DEBUG("Seek to {} seconds", seekPos);
                // }
            } else {
                // utils::highPrecisionSleep(1); // 1ms
            }
        }
    });

    // 主线程监测时长
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - testStart)
                .count() >= TEST_DURATION_SEC)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 停止
    running = false;
    if (audioThread.joinable())
        audioThread.join();
    if (videoThread.joinable())
        videoThread.join();

    // 输出统计
    LOG_INFO("\n测试完成");
    LOG_INFO("音频帧数: {} ({:.2f} fps)", audioCount.load(), audioFPS.getFPS());
    LOG_INFO("视频帧数: {} ({:.2f} fps)", videoCount.load(), videoFPS.getFPS());

    LOG_INFO("音频帧PTS: {}", lastAudioPts);
    LOG_INFO("视频帧PTS: {}", lastVideoPts);

    manager.stopDecode();
    manager.close();
    avformat_network_deinit();
    return 0;
}