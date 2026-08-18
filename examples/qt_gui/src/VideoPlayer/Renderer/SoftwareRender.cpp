#include "SoftwareRender.h"
#include <QDebug>
#include <QOpenGLContext>
#include <chrono>
#include <mutex>

namespace {
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RG8
#define GL_RG8 0x822B
#endif

    struct TextureFormatCompat {
        GLenum rgbInternal = GL_RGB8;
        GLenum rgbaInternal = GL_RGBA8;
        GLenum singleInternal = GL_RED;
        GLenum singleData = GL_RED;
        GLenum dualInternal = GL_RG;
        GLenum dualData = GL_RG;
        bool uvUseRAChannel = false; // true: UV存储在LUMINANCE_ALPHA，需要在shader中使用.ra
    };

    bool hasTextureRGExtension(QOpenGLContext *context)
    {
        return context->hasExtension(QByteArrayLiteral("GL_ARB_texture_rg")) ||
               context->hasExtension(QByteArrayLiteral("GL_EXT_texture_rg"));
    }

    const TextureFormatCompat &getTextureFormatCompat()
    {
        static std::mutex s_probeMutex;
        static bool s_probed = false;
        static TextureFormatCompat s_cachedCompat{};

        std::lock_guard<std::mutex> lock(s_probeMutex);
        if (s_probed) {
            return s_cachedCompat;
        }

        const TextureFormatCompat profileModernSized{ GL_RGB8, GL_RGBA8, GL_R8, GL_RED,
                                                      GL_RG8, GL_RG, false };
        const TextureFormatCompat profileModernUnsized{ GL_RGB, GL_RGBA, GL_RED, GL_RED,
                                                        GL_RG, GL_RG, false };
        const TextureFormatCompat profileLegacy{
            GL_RGB, GL_RGBA, GL_LUMINANCE, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, true
        };

        auto *context = QOpenGLContext::currentContext();
        if (!context) {
            s_cachedCompat = profileLegacy;
            s_probed = true;
            qWarning() << QStringLiteral(
                "[SoftwareRender] No current OpenGL context; fallback to legacy texture profile.");
            return s_cachedCompat;
        }

        const QSurfaceFormat fmt = context->format();
        const int major = fmt.majorVersion();
        const int minor = fmt.minorVersion();
        const bool isGles = context->isOpenGLES();
        const bool hasTextureRG = hasTextureRGExtension(context);

        // 使用上下文能力判定，避免依赖glGetError队列带来的不确定性。
        if (isGles) {
            if (major >= 3) {
                s_cachedCompat = profileModernSized;
                qDebug()
                    << QStringLiteral(
                           "[SoftwareRender] Texture profile selected: modern-sized (OpenGL ES %1.%2)")
                           .arg(major)
                           .arg(minor);
            } else {
                s_cachedCompat = profileLegacy;
                qDebug() << QStringLiteral(
                                "[SoftwareRender] Texture profile selected: legacy-luminance (OpenGL "
                                "ES %1.%2)")
                                .arg(major)
                                .arg(minor);
            }
        } else {
            if (major >= 3) {
                s_cachedCompat = profileModernSized;
                qDebug()
                    << QStringLiteral(
                           "[SoftwareRender] Texture profile selected: modern-sized (OpenGL %1.%2, "
                           "texture_rg=%3)")
                           .arg(major)
                           .arg(minor)
                           .arg(hasTextureRG);
            } else {
                s_cachedCompat = profileLegacy;
                qDebug()
                    << QStringLiteral(
                           "[SoftwareRender] Texture profile selected: legacy-luminance (OpenGL %1.%2)")
                           .arg(major)
                           .arg(minor);
            }
        }

        // 对极端未知版本信息的场景做兜底，保证路径确定。
        if (major <= 0) {
            s_cachedCompat = profileModernUnsized;
            qWarning() << QStringLiteral(
                "[SoftwareRender] Invalid OpenGL version info; fallback to modern-unsized profile.");
        }

        s_cachedCompat.uvUseRAChannel = !hasTextureRG;

        s_probed = true;
        return s_cachedCompat;
    }

    // 通用顶点着色器
    const char *vertexShaderSource = R"(
attribute vec4 vertexIn;
attribute vec2 textureIn;
varying highp vec2 textureOut;
void main(void)
{
    gl_Position = vertexIn;
    textureOut = textureIn;
}
)";

    // YUV420P片段着色器
    const char *yuv420pFragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D yTexture;
uniform sampler2D uTexture;
uniform sampler2D vTexture;
varying highp vec2 textureOut;
void main(void)
{
    float y = texture2D(yTexture, textureOut).r;
    float u = texture2D(uTexture, textureOut).r - 0.5;
    float v = texture2D(vTexture, textureOut).r - 0.5;
    
    // 使用BT.709标准的YUV到RGB转换矩阵
    const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
    const mat3 yuv2rgb_mat = mat3(
        1.16438356,  0.0,           1.79274107,
        1.16438356, -0.21324861, -0.53290932,
        1.16438356,  2.11240178,  0.0
    );
    
    vec3 rgb = (vec3(y, u + 0.5, v + 0.5) - yuv2rgb_ofs) * yuv2rgb_mat;
    gl_FragColor = vec4(rgb, 1.0);
}
)";

    // YUV422P片段着色器
    const char *yuv422pFragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D yTexture;
uniform sampler2D uTexture;
uniform sampler2D vTexture;
varying highp vec2 textureOut;
void main(void)
{
    float y = texture2D(yTexture, textureOut).r;
    float u = texture2D(uTexture, textureOut).r - 0.5;
    float v = texture2D(vTexture, textureOut).r - 0.5;
    
    const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
    const mat3 yuv2rgb_mat = mat3(
        1.16438356,  0.0,           1.79274107,
        1.16438356, -0.21324861, -0.53290932,
        1.16438356,  2.11240178,  0.0
    );
    
    vec3 rgb = (vec3(y, u + 0.5, v + 0.5) - yuv2rgb_ofs) * yuv2rgb_mat;
    gl_FragColor = vec4(rgb, 1.0);
}
)";

    // YUV444P片段着色器
    const char *yuv444pFragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D yTexture;
uniform sampler2D uTexture;
uniform sampler2D vTexture;
varying highp vec2 textureOut;
void main(void)
{
    float y = texture2D(yTexture, textureOut).r;
    float u = texture2D(uTexture, textureOut).r - 0.5;
    float v = texture2D(vTexture, textureOut).r - 0.5;
    
    const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
    const mat3 yuv2rgb_mat = mat3(
        1.16438356,  0.0,           1.79274107,
        1.16438356, -0.21324861, -0.53290932,
        1.16438356,  2.11240178,  0.0
    );
    
    vec3 rgb = (vec3(y, u + 0.5, v + 0.5) - yuv2rgb_ofs) * yuv2rgb_mat;
    gl_FragColor = vec4(rgb, 1.0);
}
)";

    // NV12片段着色器
    const char *nv12FragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D yTexture;
uniform sampler2D uvTexture;
varying highp vec2 textureOut;
void main(void)
{
    float y = texture2D(yTexture, textureOut).r;
#ifdef USE_UV_RA_CHANNEL
    vec2 uv = texture2D(uvTexture, textureOut).ra;
#else
    vec2 uv = texture2D(uvTexture, textureOut).rg;
#endif
    
    // 常量偏移和转换矩阵
    const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
    const mat3 yuv2rgb_mat = mat3(
        1.16438356,  0.0,           1.79274107,
        1.16438356, -0.21324861, -0.53290932,
        1.16438356,  2.11240178,  0.0
    );
    
    // YUV到RGB的转换
    vec3 rgb = (vec3(y, uv.r, uv.g) - yuv2rgb_ofs) * yuv2rgb_mat;
    gl_FragColor = vec4(rgb, 1.0);
}
)";

    // NV21片段着色器
    const char *nv21FragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D yTexture;
uniform sampler2D uvTexture;
varying highp vec2 textureOut;
void main(void)
{
    float y = texture2D(yTexture, textureOut).r;
#ifdef USE_UV_RA_CHANNEL
    vec2 vu = texture2D(uvTexture, textureOut).ra;
#else
    vec2 vu = texture2D(uvTexture, textureOut).rg;
#endif
    
    const vec3 yuv2rgb_ofs = vec3(0.0625, 0.5, 0.5);
    const mat3 yuv2rgb_mat = mat3(
        1.16438356,  0.0,           1.79274107,
        1.16438356, -0.21324861, -0.53290932,
        1.16438356,  2.11240178,  0.0
    );
    
    // NV21: V在前, U在后
    vec3 rgb = (vec3(y, vu.g, vu.r) - yuv2rgb_ofs) * yuv2rgb_mat;
    gl_FragColor = vec4(rgb, 1.0);
}
)";

    // RGB24片段着色器
    const char *rgb24FragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D rgbTexture;
varying highp vec2 textureOut;
void main(void)
{
    gl_FragColor = texture2D(rgbTexture, textureOut);
}
)";

    // BGR24片段着色器
    const char *bgr24FragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D rgbTexture;
varying highp vec2 textureOut;
void main(void)
{
    vec3 bgr = texture2D(rgbTexture, textureOut).rgb;
    gl_FragColor = vec4(bgr.b, bgr.g, bgr.r, 1.0);
}
)";

    // RGBA片段着色器
    const char *rgbaFragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D rgbTexture;
varying highp vec2 textureOut;
void main(void)
{
    gl_FragColor = texture2D(rgbTexture, textureOut);
}
)";

    // BGRA片段着色器
    const char *bgraFragmentShader = R"(
#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D rgbTexture;
varying highp vec2 textureOut;
void main(void)
{
    vec4 bgra = texture2D(rgbTexture, textureOut);
    gl_FragColor = vec4(bgra.b, bgra.g, bgra.r, bgra.a);
}
)";

} // namespace

SoftwareRender::SoftwareRender()
    : VideoRender()
{
}

SoftwareRender::~SoftwareRender()
{
    cleanup();
}

QString SoftwareRender::renderName() const
{
    return QStringLiteral("Software OpenGL Render");
}

bool SoftwareRender::initRenderVbo(const bool horizontal, const bool vertical)
{
    initDefaultVBO(vbo_, horizontal, vertical);
    return true;
}

bool SoftwareRender::initRenderShader(const decoder_sdk::Frame &frame)
{
    if (!initializeShaders(frame.pixelFormat())) {
        qWarning() << QStringLiteral("[SoftwareRender] Failed to initialize shaders!");
        return false;
    }

    return true;
}

bool SoftwareRender::initRenderTexture(const decoder_sdk::Frame &frame)
{
    return createTextures(frame.width(), frame.height(), frame.pixelFormat());
}

bool SoftwareRender::initInteropsResource(const decoder_sdk::Frame &frame)
{
    // software decode frame not need hardware interopt
    return true;
}

bool SoftwareRender::interopToOpenGL(const decoder_sdk::Frame &frame)
{
    if (!frame.isValid()) {
        return false;
    }

    // 上传纹理数据到next纹理
    const auto currentFormat = frame.pixelFormat();
    if (isYUVFormat(currentFormat)) {
        uploadYUVTextures(frame);
    } else if (isRGBFormat(currentFormat)) {
        uploadRGBTexture(frame);
    }

    return true;
}

bool SoftwareRender::renderToFbo(const decoder_sdk::Frame &frame)
{
    return drawFrame(textures_, frame.pixelFormat());
}

void SoftwareRender::cleanupAllResources()
{
    cleanup();
}

bool SoftwareRender::initializeShaders(decoder_sdk::ImageFormat format)
{
    program_.removeAllShaders();

    // 添加顶点着色器
    if (!program_.addCacheableShaderFromSourceCode(QOpenGLShader::Vertex, getVertexShader())) {
        qDebug() << QStringLiteral("[SoftwareRender] Failed to compile vertex shader: %1")
                        .arg(program_.log());
        return false;
    }

    // 根据纹理格式能力动态注入宏，兼容ES2/ANGLE的LUMINANCE_ALPHA回退路径。
    QByteArray fragmentSource;
    const TextureFormatCompat formatCompat = getTextureFormatCompat();
    if ((format == decoder_sdk::ImageFormat::kNV12 || format == decoder_sdk::ImageFormat::kNV21) &&
        formatCompat.uvUseRAChannel) {
        fragmentSource.append("#define USE_UV_RA_CHANNEL 1\n");
    }
    fragmentSource.append(getFragmentShader(format));

    // 添加片段着色器
    if (!program_.addCacheableShaderFromSourceCode(QOpenGLShader::Fragment,
                                                   fragmentSource.constData())) {
        qDebug() << QStringLiteral("[SoftwareRender] Failed to compile fragment shader: %1")
                        .arg(program_.log());
        return false;
    }

    // 链接着色器程序
    if (!program_.link()) {
        qDebug() << QStringLiteral("[SoftwareRender] Failed to link shader program: %1")
                        .arg(program_.log());
        return false;
    }

    qDebug() << QStringLiteral("[SoftwareRender] Shaders initialized successfully for format")
             << static_cast<int>(format);
    return true;
}

void SoftwareRender::cleanup()
{
    clearTextures();

    if (vbo_.isCreated()) {
        vbo_.destroy();
    }

    texturesCreated_ = false;
}

void SoftwareRender::clearTextures()
{
    auto clearTextureSet = [this](TextureSet &texSet) {
        if (texSet.yTexture) {
            glDeleteTextures(1, &texSet.yTexture);
            texSet.yTexture = 0;
        }
        if (texSet.uTexture) {
            glDeleteTextures(1, &texSet.uTexture);
            texSet.uTexture = 0;
        }
        if (texSet.vTexture) {
            glDeleteTextures(1, &texSet.vTexture);
            texSet.vTexture = 0;
        }
        if (texSet.uvTexture) {
            glDeleteTextures(1, &texSet.uvTexture);
            texSet.uvTexture = 0;
        }
    };

    clearTextureSet(textures_);
    texturesCreated_ = false;
}

bool SoftwareRender::uploadYUVTextures(const decoder_sdk::Frame &frame)
{
    if (!texturesCreated_) {
        qDebug() << QStringLiteral("[SoftwareRender] Textures not created");
        return false;
    }

    const int width = frame.width();
    const int height = frame.height();
    const int yLinesize = frame.linesize(0);
    const auto currentForamt = frame.pixelFormat();
    const TextureFormatCompat formatCompat = getTextureFormatCompat();

    // 设置像素解包参数
    glPixelStorei(GL_UNPACK_ROW_LENGTH, yLinesize);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (currentForamt == decoder_sdk::ImageFormat::kNV12 ||
        currentForamt == decoder_sdk::ImageFormat::kNV21) {
        // NV12/NV21格式：Y平面 + UV交错平面

        // 上传Y平面到next纹理
        glBindTexture(GL_TEXTURE_2D, textures_.yTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, formatCompat.singleData,
                        GL_UNSIGNED_BYTE, frame.data(0));

        // 上传UV平面
        const int uvLinesize = frame.linesize(1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, uvLinesize / 2); // UV是2字节一组
        glBindTexture(GL_TEXTURE_2D, textures_.uvTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, formatCompat.dualData,
                        GL_UNSIGNED_BYTE, frame.data(1));

    } else {
        // YUV420P/422P/444P格式：分离的Y、U、V平面

        // 上传Y平面
        glBindTexture(GL_TEXTURE_2D, textures_.yTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, formatCompat.singleData,
                        GL_UNSIGNED_BYTE, frame.data(0));

        // 计算UV平面尺寸
        int uvWidth = width;
        int uvHeight = height;
        if (currentForamt == decoder_sdk::ImageFormat::kYUV420P) {
            uvWidth /= 2;
            uvHeight /= 2;
        } else if (currentForamt == decoder_sdk::ImageFormat::kYUV422P) {
            uvWidth /= 2;
        }
        // YUV444P保持原尺寸

        // 上传U平面
        const int uLinesize = frame.linesize(1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, uLinesize);
        glBindTexture(GL_TEXTURE_2D, textures_.uTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, formatCompat.singleData,
                        GL_UNSIGNED_BYTE, frame.data(1));

        // 上传V平面
        const int vLinesize = frame.linesize(2);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, vLinesize);
        glBindTexture(GL_TEXTURE_2D, textures_.vTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, formatCompat.singleData,
                        GL_UNSIGNED_BYTE, frame.data(2));
    }

    // 恢复默认设置
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool SoftwareRender::uploadRGBTexture(const decoder_sdk::Frame &frame)
{
    if (!texturesCreated_) {
        qDebug() << QStringLiteral("[SoftwareRender] Textures not created");
        return false;
    }

    const int width = frame.width();
    const int height = frame.height();
    const int linesize = frame.linesize(0);
    const auto currentForamt = frame.pixelFormat();

    GLenum format = GL_RGB;
    int bytesPerPixel = 3;
    if (currentForamt == decoder_sdk::ImageFormat::kRGBA ||
        currentForamt == decoder_sdk::ImageFormat::kBGRA) {
        format = GL_RGBA;
        bytesPerPixel = 4;
    }

    // 设置像素解包参数
    glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize / bytesPerPixel);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const int expectedMinLinesize = width * bytesPerPixel;
    if (linesize == expectedMinLinesize) {
        // 无填充，使用默认设置
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    } else {
        // 有填充，需要设置行长度
        glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize / bytesPerPixel);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }

    glBindTexture(GL_TEXTURE_2D, textures_.yTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, frame.data(0));

    // 恢复默认设置
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

bool SoftwareRender::createTextures(int width, int height, decoder_sdk::ImageFormat format)
{
    // 清理遗留错误，避免把上游错误误报到当前纹理创建阶段。
    while (glGetError() != GL_NO_ERROR) {
    }

    clearTextures();
    const TextureFormatCompat formatCompat = getTextureFormatCompat();

    if (isYUVFormat(format)) {
        // 创建Y纹理
        glGenTextures(1, &textures_.yTexture);
        glBindTexture(GL_TEXTURE_2D, textures_.yTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, formatCompat.singleInternal, width, height, 0,
                     formatCompat.singleData, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (format == decoder_sdk::ImageFormat::kNV12 ||
            format == decoder_sdk::ImageFormat::kNV21) {
            // 创建UV交错纹理
            glGenTextures(1, &textures_.uvTexture);
            glBindTexture(GL_TEXTURE_2D, textures_.uvTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, formatCompat.dualInternal, width / 2, height / 2, 0,
                         formatCompat.dualData, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

        } else {
            // 计算UV平面尺寸
            int uvWidth = width;
            int uvHeight = height;
            if (format == decoder_sdk::ImageFormat::kYUV420P) {
                uvWidth /= 2;
                uvHeight /= 2;
            } else if (format == decoder_sdk::ImageFormat::kYUV422P) {
                uvWidth /= 2;
            }

            // 创建U纹理
            glGenTextures(1, &textures_.uTexture);
            glBindTexture(GL_TEXTURE_2D, textures_.uTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, formatCompat.singleInternal, uvWidth, uvHeight, 0,
                         formatCompat.singleData, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);

            // 创建V纹理
            glGenTextures(1, &textures_.vTexture);
            glBindTexture(GL_TEXTURE_2D, textures_.vTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, formatCompat.singleInternal, uvWidth, uvHeight, 0,
                         formatCompat.singleData, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    } else if (isRGBFormat(format)) {
        GLenum internalFormat = formatCompat.rgbInternal;
        GLenum dataFormat = GL_RGB;
        if (format == decoder_sdk::ImageFormat::kRGBA ||
            format == decoder_sdk::ImageFormat::kBGRA) {
            internalFormat = formatCompat.rgbaInternal;
            dataFormat = GL_RGBA;
        }

        glGenTextures(1, &textures_.yTexture);
        glBindTexture(GL_TEXTURE_2D, textures_.yTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, dataFormat,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    texturesCreated_ = true;
    return true;
}

const char *SoftwareRender::getVertexShader() const
{
    return vertexShaderSource;
}

const char *SoftwareRender::getFragmentShader(decoder_sdk::ImageFormat format) const
{
    switch (format) {
        case decoder_sdk::ImageFormat::kNV12:
            return nv12FragmentShader;
        case decoder_sdk::ImageFormat::kNV21:
            return nv21FragmentShader;
        case decoder_sdk::ImageFormat::kYUV420P:
            return yuv420pFragmentShader;
        case decoder_sdk::ImageFormat::kYUV422P:
            return yuv422pFragmentShader;
        case decoder_sdk::ImageFormat::kYUV444P:
            return yuv444pFragmentShader;
        case decoder_sdk::ImageFormat::kRGB24:
            return rgb24FragmentShader;
        case decoder_sdk::ImageFormat::kBGR24:
            return bgr24FragmentShader;
        case decoder_sdk::ImageFormat::kRGBA:
            return rgbaFragmentShader;
        case decoder_sdk::ImageFormat::kBGRA:
            return bgraFragmentShader;
        default:
            return rgb24FragmentShader; // 默认
    }
}

bool SoftwareRender::isSupportedFormat(decoder_sdk::ImageFormat format) const
{
    return format == decoder_sdk::ImageFormat::kNV12 || format == decoder_sdk::ImageFormat::kNV21 ||
           format == decoder_sdk::ImageFormat::kYUV420P ||
           format == decoder_sdk::ImageFormat::kYUV422P ||
           format == decoder_sdk::ImageFormat::kYUV444P ||
           format == decoder_sdk::ImageFormat::kRGB24 ||
           format == decoder_sdk::ImageFormat::kBGR24 ||
           format == decoder_sdk::ImageFormat::kRGBA || format == decoder_sdk::ImageFormat::kBGRA;
}

bool SoftwareRender::isYUVFormat(decoder_sdk::ImageFormat format) const
{
    return format == decoder_sdk::ImageFormat::kNV12 || format == decoder_sdk::ImageFormat::kNV21 ||
           format == decoder_sdk::ImageFormat::kYUV420P ||
           format == decoder_sdk::ImageFormat::kYUV422P ||
           format == decoder_sdk::ImageFormat::kYUV444P;
}

bool SoftwareRender::isRGBFormat(decoder_sdk::ImageFormat format) const
{
    return format == decoder_sdk::ImageFormat::kRGB24 ||
           format == decoder_sdk::ImageFormat::kBGR24 ||
           format == decoder_sdk::ImageFormat::kRGBA || format == decoder_sdk::ImageFormat::kBGRA;
}

bool SoftwareRender::drawFrame(const TextureSet &textures, decoder_sdk::ImageFormat format)
{
    program_.bind();
    vbo_.bind();

    // 设置纹理uniform
    if (isYUVFormat(format)) {
        if (format == decoder_sdk::ImageFormat::kNV12 ||
            format == decoder_sdk::ImageFormat::kNV21) {
            // NV12/NV21格式
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textures.yTexture);
            program_.setUniformValue("yTexture", 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, textures.uvTexture);
            program_.setUniformValue("uvTexture", 1);
        } else {
            // YUV420P/422P/444P格式
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textures.yTexture);
            program_.setUniformValue("yTexture", 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, textures.uTexture);
            program_.setUniformValue("uTexture", 1);

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, textures.vTexture);
            program_.setUniformValue("vTexture", 2);
        }
    } else if (isRGBFormat(format)) {
        // RGB格式
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures.yTexture); // 复用yTexture_存储RGB数据
        program_.setUniformValue("rgbTexture", 0);
    }

    // 设置顶点属性
    program_.enableAttributeArray("vertexIn");
    program_.enableAttributeArray("textureIn");
    program_.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 2, 0);
    program_.setAttributeBuffer("textureIn", GL_FLOAT, 2 * 4 * sizeof(GLfloat), 2, 0);

    // 绘制
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    program_.disableAttributeArray("vertexIn");
    program_.disableAttributeArray("textureIn");
    program_.release();
    vbo_.release();

    return true;
}
