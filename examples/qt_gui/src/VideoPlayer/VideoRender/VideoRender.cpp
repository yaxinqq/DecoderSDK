#include "VideoRender.h"

#pragma region FboQueue
/**
 * @brief 带最大容量的循环缓冲池队列
 */
class FboQueue {
public:
    explicit FboQueue(int capacity = 1);

    // 获得一个可读的元素，可选择性地进行交换，交换后，exchange会在对尾的位置
    QSharedPointer<QOpenGLFramebufferObject> getReadable(
        QSharedPointer<QOpenGLFramebufferObject> exchange = nullptr);

    // 获取一个可写的元素
    QSharedPointer<QOpenGLFramebufferObject> getWritable(
        const QSize &size,
        const QOpenGLFramebufferObjectFormat &fmt = QOpenGLFramebufferObjectFormat());

    int size() const;
    int capacity() const;
    bool empty() const;
    bool full() const;
    void clear();

private:
    QList<QSharedPointer<QOpenGLFramebufferObject>> queue_;
    int maxSize_ = 2;
    int front_ = 0; // 读指针
    int rear_ = 0;  // 写指针
    int size_ = 0;
};

FboQueue::FboQueue(int capacity) : maxSize_(capacity), front_(0), rear_(0), size_(0)
{
    if (capacity <= 0) {
        maxSize_ = 2;
    }
    queue_.resize(maxSize_);
}

QSharedPointer<QOpenGLFramebufferObject> FboQueue::getReadable(
    QSharedPointer<QOpenGLFramebufferObject> exchange)
{
    if (size_ == 0)
        return nullptr;

    QSharedPointer<QOpenGLFramebufferObject> result = queue_[front_];

    if (exchange) {
        // 原地交换：将exchange放到刚刚读取的位置
        queue_[front_] = exchange;
        front_ = (front_ + 1) % maxSize_;
        // size_保持不变，因为是1:1交换
    } else {
        // 没有exchange，正常出队
        front_ = (front_ + 1) % maxSize_;
        --size_;
    }

    return result;
}

QSharedPointer<QOpenGLFramebufferObject> FboQueue::getWritable(
    const QSize &size, const QOpenGLFramebufferObjectFormat &fmt)
{
    if (size_ == maxSize_) {
        // 队列满时，覆盖最旧的元素
        front_ = (front_ + 1) % maxSize_;
    } else {
        ++size_;
    }

    int writeIdx = rear_;
    rear_ = (rear_ + 1) % maxSize_;

    if (!queue_[writeIdx] || queue_[writeIdx]->size() != size) {
        queue_[writeIdx] = QSharedPointer<QOpenGLFramebufferObject>::create(size, fmt);
    }

    return queue_[writeIdx];
}

int FboQueue::size() const
{
    return size_;
}

int FboQueue::capacity() const
{
    return maxSize_;
}

bool FboQueue::empty() const
{
    return size_ == 0;
}

bool FboQueue::full() const
{
    return size_ == maxSize_;
}

void FboQueue::clear()
{
    for (auto &item : queue_)
        item.clear();
    front_ = 0;
    rear_ = 0;
    size_ = 0;
}
#pragma endregion

VideoRender::VideoRender()
    : fboQueue_(new FboQueue(3)), initialized_(false), fboDrawResourcesInitialized_(false)
{
}

VideoRender::~VideoRender() = default;

void VideoRender::initialize(const decoder_sdk::Frame &frame, const bool horizontal,
                             const bool vertical)
{
    if (initialized_) {
        return;
    }

    initializeOpenGLFunctions();

    // 调用子类的初始化方法
    initRenderVbo(horizontal, vertical);
    initRenderShader(frame);
    initRenderTexture(frame);
    initInteropsResource(frame);

    // 初始化FBO绘制资源
    initializeFboDrawResources();

    initialized_ = true;
}

void VideoRender::render(const decoder_sdk::Frame &frame)
{
    if (!initialized_ || !frame.isValid()) {
        return;
    }

    // 获取FBO尺寸和格式
    QSize fboSize = getDefaultFboSize();
    auto fmt = getDefaultFboFormat();

    // 获取可写的FBO
    QSharedPointer<QOpenGLFramebufferObject> newFbo = fboQueue_->getWritable(fboSize, fmt);
    if (!newFbo) {
        return;
    }

    // 绑定FBO并让子类渲染到其中
    newFbo->bind();
    renderFrame(frame);
    newFbo->release();

    // 更新当前FBO
    updateCurrentFbo(newFbo);
}

void VideoRender::draw()
{
    if (!initialized_ || !curFbo_) {
        return;
    }

    drawFboToScreen(curFbo_);
}

QSharedPointer<QOpenGLFramebufferObject> VideoRender::getFrameBuffer()
{
    if (!curFbo_) {
        return nullptr;
    }

    // 深拷贝当前FBO
    auto copyFbo = createFbo(curFbo_->size(), curFbo_->format());
    if (!copyFbo) {
        return nullptr;
    }

    // 使用blit进行深拷贝
    QOpenGLFramebufferObject::blitFramebuffer(copyFbo.get(), curFbo_.get());

    return copyFbo;
}

void VideoRender::initializeFboDrawResources()
{
    if (fboDrawResourcesInitialized_) {
        return;
    }

    // 简单的全屏quad shader
    const char *vertexShader = R"(
        attribute vec4 position;
        attribute vec2 texCoord;
        varying vec2 vTexCoord;
        void main() {
            gl_Position = position;
            vTexCoord = texCoord;
        }
    )";

    const char *fragmentShader = R"(
        #ifdef GL_ES
        precision mediump float;
        #endif
        uniform sampler2D texture;
        varying vec2 vTexCoord;
        void main() {
            gl_FragColor = texture2D(texture, vTexCoord);
        }
    )";

    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    fboDrawProgram_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    fboDrawProgram_.link();

    // 全屏quad的顶点数据
    GLfloat vertices[] = {// 位置坐标        // 纹理坐标
                          -1.0f, 1.0f,  0.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f,
                          -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f};

    fboDrawVbo_.create();
    fboDrawVbo_.bind();
    fboDrawVbo_.allocate(vertices, sizeof(vertices));
    fboDrawVbo_.release();

    fboDrawResourcesInitialized_ = true;
}

void VideoRender::drawFboToScreen(QSharedPointer<QOpenGLFramebufferObject> fbo)
{
    if (!fbo || !fboDrawResourcesInitialized_) {
        return;
    }

    fboDrawProgram_.bind();
    fboDrawVbo_.bind();

    fboDrawProgram_.enableAttributeArray("position");
    fboDrawProgram_.enableAttributeArray("texCoord");
    fboDrawProgram_.setAttributeBuffer("position", GL_FLOAT, 0, 2, 4 * sizeof(GLfloat));
    fboDrawProgram_.setAttributeBuffer("texCoord", GL_FLOAT, 2 * sizeof(GLfloat), 2,
                                       4 * sizeof(GLfloat));

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

void VideoRender::updateCurrentFbo(QSharedPointer<QOpenGLFramebufferObject> newFbo)
{
    if (curFbo_) {
        // 将当前FBO放入队列（与队头交换，成为队尾）
        fboQueue_->getReadable(curFbo_);
    }
    curFbo_ = newFbo;
}