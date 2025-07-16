#include "VideoRender.h"

namespace {
	const char *vsrc = R"(
    #ifdef GL_ES
        precision mediump float;
    #endif

        attribute vec4 position;
        attribute vec2 texCoord;
        varying vec2 vTexCoord;
        void main() {
            gl_Position = position;
            vTexCoord = texCoord;
        }
    )";

	const char *fsrc = R"(
    #ifdef GL_ES
        precision mediump float;
    #endif

        uniform sampler2D texture;
        varying vec2 vTexCoord;
        void main() {
            gl_FragColor = texture2D(texture, vTexCoord);
        }
    )";
} // namespace

VideoRender::VideoRender()
	: initialized_{ false }, fboDrawResourcesInitialized_{ false },
	curFboIndex_{ 0 }, pendingFence_{ nullptr }, supportsGlFenceSync_{ false }
{
	for (int i = 0; i < 3; ++i) {
		fbos_[i] = nullptr;
		fboUsed_[i] = false;
	}
}

VideoRender::~VideoRender()
{
	for (int i = 0; i < 3; ++i) {
		fbos_[i].reset();
	}
	fboDrawVbo_.destroy();
	if (pendingFence_) {
		glDeleteSync(pendingFence_);
		pendingFence_ = nullptr;
	}
}

void VideoRender::initialize(const std::shared_ptr<decoder_sdk::Frame> &frame, const bool horizontal,
	const bool vertical)
{
	if (initialized_.load() || !frame || !frame->isValid()) {
		return;
	}

	initializeOpenGLFunctions();

	// 初始化FBO绘制资源
	if (!fboDrawResourcesInitialized_.load()) {
		fboDrawResourcesInitialized_ = initializeFboDrawResources(QSize(frame->width(), frame->height()));
	}
	if (!fboDrawResourcesInitialized_.load())
		return;

	// 调用子类的初始化方法
	if (!initRenderVbo(horizontal, vertical))
		return;

	if (!initRenderShader(*frame))
		return;

	if (!initRenderTexture(*frame))
		return;

	if (!initInteropsResource(*frame))
		return;

	QOpenGLContext *context = QOpenGLContext::currentContext();
	if (context) {
		supportsGlFenceSync_ = context->hasExtension(QByteArrayLiteral("GL_ARB_sync")) || context->hasExtension("GL_OES_EGL_sync");
	}

	initialized_.store(true);
}

void VideoRender::render(const std::shared_ptr<decoder_sdk::Frame> &frame)
{
    if (!frame->isValid() || !isValid()) {
        return;
    }

    // 查询是否需要同步
    pollGpuFence();

    // 获得空闲的FBO索引
    int freeIndex = getFreeFboIndex();
    if (freeIndex < 0) {
        qWarning() << "[VideoRender] No free FBO available, skipping frame";
        return;
    }

    // 绑定FBO并让子类渲染到其中
    QSharedPointer<QOpenGLFramebufferObject> targetFbo = fbos_[freeIndex];
    targetFbo->bind();
    glViewport(0, 0, frame->width(), frame->height());
    bool success = renderFrame(*frame);
    targetFbo->release();

    // 更新当前FBO
    if (success) {
        if (supportsGlFenceSync_) {
            if (pendingFence_) glDeleteSync(pendingFence_);
            pendingFence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        else {
            glFinish();
        }
        
        QMutexLocker l(&mutex_);
        nextFboIndex_ = freeIndex;
    }

    cleanupRenderResources();
}

void VideoRender::pollGpuFence()
{
    if (nextFboIndex_ < 0 || nextFboIndex_ >= 3)
        return;

    bool ready = true;
    if (supportsGlFenceSync_ && pendingFence_) {
        GLenum result = glClientWaitSync(pendingFence_, 0, 0);
        ready = (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED);
        if (ready) {
            glDeleteSync(pendingFence_);
            pendingFence_ = nullptr;
        }
    }

    if (ready) {
        QMutexLocker l(&mutex_);
        int old = curFboIndex_;
        curFboIndex_ = nextFboIndex_;
        
        // 标记新FBO为使用中，释放旧FBO
        fboUsed_[curFboIndex_] = true;
        fboUsed_[old] = false;
        
        nextFboIndex_ = -1;
    }
}

void VideoRender::draw()
{
	if (!isValid()) return;
	pollGpuFence();

	QMutexLocker l(&mutex_);
	int index = curFboIndex_;
	if (fbos_[index] && fbos_[index]->isValid()) {
		drawFbo(fbos_[index]);
	}
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::getFrameBuffer()
{
	QMutexLocker l(&mutex_);
	QSharedPointer<QOpenGLFramebufferObject> src = fbos_[curFboIndex_];
	if (!src) return nullptr;

	// 深拷贝当前FBO
	auto copyFbo = createFbo(src->size(), src->format());
	if (!copyFbo) return nullptr;
	QOpenGLFramebufferObject::blitFramebuffer(copyFbo.get(), src.get());
	return copyFbo;
}

bool VideoRender::isValid() const
{
	return initialized_.load() && fboDrawResourcesInitialized_.load();
}

int VideoRender::getFreeFboIndex() const
{
	for (int i = 0; i < 3; ++i) {
		if (!fboUsed_[i]) return i;
	}
	return -1;
}

bool VideoRender::initializeFboDrawResources(const QSize &size)
{
	fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vsrc);
	fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fsrc);
	fboDrawProgram_.link();

	// 全屏quad的顶点数据（交错式布局）
	const GLfloat vertices[] = {
		-1.0f, 1.0f,  0.0f, 1.0f,
		 1.0f, 1.0f,  1.0f, 1.0f,
		-1.0f, -1.0f, 0.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 0.0f
	};

	fboDrawVbo_.create();
	fboDrawVbo_.bind();
	fboDrawVbo_.allocate(vertices, sizeof(vertices));
	fboDrawVbo_.release();

	// 创建FBO
	for (int i = 0; i < 3; ++i) {
		fbos_[i] = createFbo(size, {});
		if (!fbos_[i] || !fbos_[i]->isValid()) return false;
		fboUsed_[i] = false;
	}

	curFboIndex_ = 0;
	nextFboIndex_ = -1;
	fboUsed_[curFboIndex_] = true;

	return true;
}

void VideoRender::drawFbo(QSharedPointer<QOpenGLFramebufferObject> fbo)
{
	if (!fbo || !fboDrawResourcesInitialized_.load()) 
		return;

	fboDrawProgram_.bind();
	fboDrawVbo_.bind();

	fboDrawProgram_.enableAttributeArray("position");
	fboDrawProgram_.enableAttributeArray("texCoord");
	fboDrawProgram_.setAttributeBuffer("position", GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
	fboDrawProgram_.setAttributeBuffer("texCoord", GL_FLOAT, 2 * sizeof(GLfloat), 2, 4 * sizeof(GLfloat));

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, fbo->texture());
	fboDrawProgram_.setUniformValue("texture", 0);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	fboDrawProgram_.disableAttributeArray("position");
	fboDrawProgram_.disableAttributeArray("texCoord");
	fboDrawVbo_.release();
	fboDrawProgram_.release();
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::createFbo(
	const QSize &size, const QOpenGLFramebufferObjectFormat &fmt)
{
	if (!size.isValid()) {
		return nullptr;
	}
	return QSharedPointer<QOpenGLFramebufferObject>::create(size, fmt);
}

void VideoRender::initDefaultVBO(QOpenGLBuffer &vbo, const bool horizontal,
	const bool vertical) const
{
	// 设置顶点数据
	const GLfloat points[] = {
		// 位置坐标
		-1.0f,
		1.0f,
		1.0f,
		1.0f,
		-1.0f,
		-1.0f,
		1.0f,
		-1.0f,

		// 纹理坐标
		horizontal ? 1.0f : 0.0f,
		vertical ? 1.0f : 0.0f,
		horizontal ? 0.0f : 1.0f,
		vertical ? 1.0f : 0.0f,
		horizontal ? 1.0f : 0.0f,
		vertical ? 0.0f : 1.0f,
		horizontal ? 0.0f : 1.0f,
		vertical ? 0.0f : 1.0f,
	};

	vbo.create();
	vbo.bind();
	vbo.allocate(points, sizeof(points));
	vbo.release();
}

void VideoRender::clearGL()
{
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}
