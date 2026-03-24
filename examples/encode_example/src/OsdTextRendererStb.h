#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 使用 stb_truetype 在 CPU 侧将 UTF-8 文本光栅化为 RGBA 位图。
 *
 * 设计目标：
 * - 支持中文时间戳（年/月/日）。
 * - 每秒渲染一次即可，避免每帧生成。
 * - 输出 RGBA8（透明背景 + 白色字体），便于作为 sampled image 叠加渲染。
 */
class OsdTextRendererStb {
public:
    // 位图数据
    struct RgbaBitmap {
        // 位图宽度
        uint32_t width = 0;
        // 位图高度
        uint32_t height = 0;
        // 图像数据
        std::vector<uint8_t> rgba;
    };

public:
    OsdTextRendererStb();
    ~OsdTextRendererStb();

    /**
     * @brief 加载字体文件。
     *
     * 支持 .ttf 与 .ttc（ttc 取 index=0）。
     * @param fontPath 字体文件路径。
     */
    bool loadFontFile(const std::string &fontPath);

    /**
     * @brief 尝试从常见 Windows 字体路径加载中文字体。
     */
    bool tryLoadDefaultChineseFont();

    /**
     * @brief 渲染 UTF-8 文本到 RGBA 位图。
     *
     * @param utf8Text UTF-8 文本。
     * @param pixelHeight 字体像素高度。
     * @param paddingPx 四周留白。
     */
    RgbaBitmap renderText(const std::string &utf8Text, int pixelHeight, int paddingPx) const;

private:
    struct FontImpl;
    FontImpl *impl_ = nullptr;
};

