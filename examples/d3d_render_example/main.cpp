#include <windows.h>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

#include "D3DRenderer.h"
#include "decodersdk/decoder_controller.h"

using namespace decoder_sdk;

// 窗口类名
const wchar_t *WINDOW_CLASS_NAME = L"D3DRenderExample";

// 全局变量
std::unique_ptr<D3DRenderer> g_renderer;
std::atomic<bool> g_running{true};
HWND g_hwnd = nullptr;
int g_windowWidth = 1280;
int g_windowHeight = 720;

// 窗口过程函数
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;

        case WM_SIZE:
            if (g_renderer && wParam != SIZE_MINIMIZED) {
                int width = LOWORD(lParam);
                int height = HIWORD(lParam);
                g_renderer->Resize(width, height);
                g_windowWidth = width;
                g_windowHeight = height;
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_running = false;
                PostQuitMessage(0);
            }
            return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// 创建窗口
bool CreateRenderWindow()
{
    HINSTANCE hInstance = GetModuleHandle(nullptr);

    // 注册窗口类
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassEx(&wc)) {
        std::cerr << "Failed to register window class" << std::endl;
        return false;
    }

    // 计算窗口大小
    RECT rect = {0, 0, g_windowWidth, g_windowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    // 创建窗口
    g_hwnd = CreateWindowEx(0, WINDOW_CLASS_NAME, L"D3D Video Render Example", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                            rect.bottom - rect.top, nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    return true;
}

// FPS计算器
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
        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();

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

// 视频渲染线程
void VideoRenderThread(DecoderController &decoder)
{
    FPSCalculator fpsCalculator;
    int frameCount = 0;

    std::cout << "Video render thread started" << std::endl;

    while (g_running) {
        Frame frame;
        if (decoder.videoQueue().pop(frame, 1) && frame.isValid()) {
            if (g_renderer) {
                // 渲染帧
                if (g_renderer->RenderFrame(frame)) {
                    g_renderer->Present();
                    fpsCalculator.update();
                    frameCount++;

                    // 每100帧输出一次统计信息
                    if (frameCount % 100 == 0) {
                        std::cout << "Rendered " << frameCount << " frames, FPS: " << std::fixed
                                  << std::setprecision(2) << fpsCalculator.getFPS()
                                  << ", PTS: " << frame.secPts() << "s" << std::endl;
                    }
                } else {
                    std::cerr << "Failed to render frame" << std::endl;
                }
            }
        }
    }

    std::cout << "Video render thread finished. Total frames: " << frameCount << std::endl;
}

// 消息循环
void MessageLoop()
{
    MSG msg = {};
    while (g_running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        // 短暂休眠以避免100%CPU占用
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main(int argc, char *argv[])
{
    // 设置控制台输出编码
    SetConsoleOutputCP(CP_UTF8);
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "D3D Video Render Example" << std::endl;
    std::cout << "========================" << std::endl;

    // 获取视频路径
    std::string videoPath;
    if (argc > 1) {
        videoPath = argv[1];
    } else {
        std::cout << "Please enter video path (or press Enter for default RTSP stream): ";
        std::getline(std::cin, videoPath);
        if (videoPath.empty()) {
            videoPath = "rtsp://admin:zhkj2501@192.168.0.71:554/ch1/stream1";
        }
    }

    std::cout << "Video path: " << videoPath << std::endl;

    // 创建窗口
    if (!CreateRenderWindow()) {
        std::cerr << "Failed to create render window" << std::endl;
        return -1;
    }

    // 创建D3D渲染器
    g_renderer = std::make_unique<D3DRenderer>();
    if (!g_renderer->Initialize(g_hwnd, g_windowWidth, g_windowHeight)) {
        std::cerr << "Failed to initialize D3D renderer" << std::endl;
        return -1;
    }

    // 创建解码器
    DecoderController decoder;

    // 注册事件监听器
    decoder.addGlobalEventListener([](EventType eventType, std::shared_ptr<EventArgs> event) {
        std::cout << "Event [" << event->source << "]: " << getEventTypeName(eventType) << " - "
                  << event->description << std::endl;
    });

    // 配置解码器
    Config config;
    config.hwAccelType = HWAccelType::kD3d11va; // 使用D3D11VA硬件加速
    config.decodeMediaType = Config::DecodeMediaType::kVideo;
    config.requireFrameInSystemMemory = false; // 不需要系统内存中的帧

    // 如果D3D11VA不可用，回退到软解码
    if (!decoder.open(videoPath, config)) {
        std::cout << "D3D11VA failed, trying software decoding..." << std::endl;
        config.hwAccelType = HWAccelType::kNone;
        config.swVideoOutFormat = ImageFormat::kRGBA;
        config.requireFrameInSystemMemory = true;

        if (!decoder.open(videoPath, config)) {
            std::cerr << "Failed to open video: " << videoPath << std::endl;
            return -1;
        }
    }

    std::cout << "Video opened successfully" << std::endl;
    std::cout << "Hardware acceleration: "
              << (config.hwAccelType == HWAccelType::kD3d11va ? "D3D11VA" : "Software")
              << std::endl;
    std::cout << "Output format: "
              << (config.swVideoOutFormat == ImageFormat::kD3d11va ? "D3D11VA" : "RGBA")
              << std::endl;

    // 启动解码
    decoder.setFrameRateControl(true); // 启用帧率控制
    decoder.setSpeed(1.0f);            // 正常播放速度
    decoder.startDecode();

    std::cout << "Decoding started. Press ESC to exit." << std::endl;

    // 启动视频渲染线程
    std::thread renderThread(VideoRenderThread, std::ref(decoder));

    // 运行消息循环
    MessageLoop();

    // 清理
    std::cout << "Shutting down..." << std::endl;
    g_running = false;

    if (renderThread.joinable()) {
        renderThread.join();
    }

    decoder.stopDecode();
    decoder.close();

    g_renderer.reset();

    if (g_hwnd) {
        DestroyWindow(g_hwnd);
    }

    std::cout << "Application finished" << std::endl;
    return 0;
}