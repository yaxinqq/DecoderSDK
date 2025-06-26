#pragma once
#include "VideoRender.h"

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLFramebufferObject>
#include <QMutex>

#include <cuda.h>
#include <cudaGL.h>

#include <condition_variable>

class Nv12Render_Cuda : public QOpenGLFunctions, public VideoRender
{
public:
	Nv12Render_Cuda(CUcontext ctx = nullptr);
	~Nv12Render_Cuda() override;

public:
	void initialize(const int width, const int height, const bool horizontal = false, const bool vertical = false) override;
	//
	void render(const decoder_sdk::Frame &frame) override;
	//
	void draw() override;

	QOpenGLFramebufferObject *getFrameBuffer(const QSize &size) override;

private:
	void clearGL();

private:
	// 纹理同步锁
	QMutex mtx_;

	// CUDA流同步
	std::condition_variable conditional_;
	std::mutex conditionalMtx_;

	std::atomic_bool copyYSucced_ = false;
	std::atomic_bool copyUVSucced_ = false;

	// CUDA的上下文和流
	CUcontext context_ = nullptr;
	CUstream copyYStream_ = nullptr;
	CUstream copyUVStream_ = nullptr;

	// CUDA的资源映射对象
	CUgraphicsResource resourceCurrentY_ = nullptr;
	CUgraphicsResource resourceCurrentUV_ = nullptr;
	CUgraphicsResource resourceNextY_ = nullptr;
	CUgraphicsResource resourceNextUV_ = nullptr;

	// 用来从CUDA中拷贝数据
	CUarray cudaArrayCurrentY_;
	CUarray cudaArrayCurrentUV_;
	CUarray cudaArrayNextY_;
	CUarray cudaArrayNextUV_;

	// 资源映射是否成功
	bool resourceCurrentYRegisteredFailed_ = false;
	bool resourceCurrentUVRegisteredFailed_ = false;
	bool resourceNextYRegisteredFailed_ = false;
	bool resourceNextUVRegisteredFailed_ = false;

	// 是否需要删除CUDA上下文
	bool needDestoryContext_ = false;

	// OpenGL的相关对象
	QOpenGLShaderProgram program_;
	QOpenGLBuffer vbo_;
	GLuint idCurrentY_ = 0, idCurrentUV_ = 0;
	GLuint idNextY_ = 0, idNextUV_ = 0;
};
