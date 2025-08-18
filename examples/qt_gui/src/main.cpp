#include "SimplePlayer.h"
#include "VideoPlayer/Commonutils.h"

#include <QApplication>

#ifdef Q_OS_WIN
#include <windows.h>
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;       // NVIDIA
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1; // AMD
}
#endif


int main(int argc, char *argv[])
{
    // 禁用高 DPI 缩放
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");

    // 允许 OpenGL 上下文资源共享
    QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);

    registerVideoMetaType();
    getCurrentGLRenderer();

    SimplePlayer w;
    w.resize(1200, 600);
    w.show();

    const auto ret = app.exec();

    // 清理GPU资源，主要是D3D11和DXVA2
    // 如果在单例析构中退出，似乎有点晚
    clearGPUResource();

    return ret;
}