#ifndef VIDEORENDER_H
#define VIDEORENDER_H

#include "decodersdk/frame.h"
#include <QOpenGLFramebufferObject>

class VideoRender
{
public:
	virtual ~VideoRender() {}

	/**
	 * @description: 初始化OpenGL上下文，编译链接shader；如果是GPU直接与OpenGL对接数据，则会分配GPU内存或注册资源
	 * @param width		 视频宽度
	 * @param height	 视频高度
	 * @param horizontal 是否水平镜像
	 * @param vertical	 是否垂直镜像
	 */
	virtual void initialize(const int width, const int height, const bool horizontal = false, const bool vertical = false) = 0;

	virtual void render(const decoder_sdk::Frame &frame) = 0;

	/**
	 * @description: 异步绘制纹理数据
	 */
	virtual void draw() = 0;

	/**
	 * @description: 将图像渲染到缓存帧中，外部负责释放QOpenGLFramebufferObject
	 */
	virtual QOpenGLFramebufferObject *getFrameBuffer(const QSize &size) { return nullptr; };
};

#endif // VIDEORENDER_H