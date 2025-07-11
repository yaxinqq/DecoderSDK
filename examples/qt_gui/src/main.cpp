#include "SimplePlayer.h"
#include "VideoPlayer/Commonutils.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    // 禁用高 DPI 缩放
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");

    // 允许 OpenGL 上下文资源共享
    QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);
    
    registerVideoMetaType();

    SimplePlayer w;
    w.resize(1200, 600);
    w.show();

    return app.exec();  
}