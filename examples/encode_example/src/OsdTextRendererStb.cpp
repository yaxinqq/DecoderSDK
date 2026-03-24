#include "OsdTextRendererStb.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
// utf8迭代器，将UTF8字符读取为对应的Unicode字符
struct Utf8Iterator {
    // 当前指针
    const uint8_t *p = nullptr;
    // 结尾指针
    const uint8_t *end = nullptr;

    explicit Utf8Iterator(const std::string &s)
        : p(reinterpret_cast<const uint8_t *>(s.data())),
          end(reinterpret_cast<const uint8_t *>(s.data() + s.size()))
    {
    }

    // 是否还有下一个字符
    bool hasNext() const
    {
        return p < end;
    }

    uint32_t next()
    {
        // 已经是结尾，返回
        if (p >= end) {
            return 0;
        }

        // 读取下一个字符，如果是单字节，直接返回
        uint32_t c = *p++;
        if (c < 0x80) {
            return c;
        }

        // 读取两字节
        if ((c >> 5) == 0x6) {
            if (p >= end)
                return 0xFFFD;
            uint32_t c2 = *p++;
            return ((c & 0x1F) << 6) | (c2 & 0x3F);
        }

        // 读取三字节
        if ((c >> 4) == 0xE) {
            if (p + 1 >= end)
                return 0xFFFD;
            uint32_t c2 = *p++;
            uint32_t c3 = *p++;
            return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        }

        // 读取四字节
        if ((c >> 3) == 0x1E) {
            if (p + 2 >= end)
                return 0xFFFD;
            uint32_t c2 = *p++;
            uint32_t c3 = *p++;
            uint32_t c4 = *p++;
            return ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) | ((c3 & 0x3F) << 6) | (c4 & 0x3F);
        }

        return 0xFFFD;
    }
};

// 读取一个文件的所以内容
static bool readFileBytes(const std::string &path, std::vector<uint8_t> &out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        return false;
    }

    const std::streamsize size = f.tellg();
    if (size <= 0) {
        return false;
    }

    out.resize(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char *>(out.data()), size);
    return true;
}
} // namespace

// 字体相关数据
struct OsdTextRendererStb::FontImpl {
    // 字体文件内容
    std::vector<uint8_t> fileData;
    // 字体信息
    stbtt_fontinfo font{};
    // 所用字体在字体文件的偏移
    int fontOffset = 0;
    // 是否已加载
    bool loaded = false;
};

OsdTextRendererStb::OsdTextRendererStb() : impl_(new FontImpl)
{
}

OsdTextRendererStb::~OsdTextRendererStb()
{
    delete impl_;
    impl_ = nullptr;
}

bool OsdTextRendererStb::loadFontFile(const std::string &fontPath)
{
    if (!impl_) {
        return false;
    }

    impl_->fileData.clear();
    impl_->loaded = false;
    impl_->fontOffset = 0;

    std::vector<uint8_t> bytes;
    if (!readFileBytes(fontPath, bytes)) {
        return false;
    }

    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
    if (offset < 0) {
        return false;
    }

    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, bytes.data(), offset)) {
        return false;
    }

    impl_->fileData = std::move(bytes);
    impl_->font = info;
    impl_->fontOffset = offset;
    impl_->loaded = true;
    return true;
}

bool OsdTextRendererStb::tryLoadDefaultChineseFont()
{
    // 系统中可能存在的字体文件
    const std::vector<std::string> candidates = {
        "C:/Windows/Fonts/msyh.ttc",   "C:/Windows/Fonts/msyh.ttf",   "C:/Windows/Fonts/msyhl.ttc",
        "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/simsun.ttc",
    };

    // 加载一个存在的
    for (const auto &p : candidates) {
        if (std::filesystem::exists(p) && loadFontFile(p)) {
            return true;
        }
    }
    return false;
}

OsdTextRendererStb::RgbaBitmap OsdTextRendererStb::renderText(const std::string &utf8Text,
                                                              int pixelHeight, int paddingPx) const
{
    RgbaBitmap out;
    if (!impl_ || !impl_->loaded || utf8Text.empty() || pixelHeight <= 0) {
        return out;
    }

    // 根据像素高度，获得对应的字体信息
    const float scale = stbtt_ScaleForPixelHeight(&impl_->font, static_cast<float>(pixelHeight));

    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&impl_->font, &ascent, &descent, &lineGap);
    (void)lineGap;

    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool first = true;

    int penX = 0;
    uint32_t prev = 0;

    // 第一次循环，先计算出对应的宽高
    Utf8Iterator itMeasure(utf8Text);
    while (itMeasure.hasNext()) {
        const uint32_t cp = itMeasure.next();

        // 得到字符的包围盒
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(&impl_->font, static_cast<int>(cp), scale, scale, &x0, &y0, &x1,
                                    &y1);

        // 在两个字符(前一个字符、当前字符)间加上空白
        const int kern =
            (prev != 0)
                ? static_cast<int>(stbtt_GetCodepointKernAdvance(
                                       &impl_->font, static_cast<int>(prev), static_cast<int>(cp)) *
                                   scale)
                : 0;

        // 得到这个新字符起点位置的宽和高
        const int gx0 = penX + kern + x0;
        const int gx1 = penX + kern + x1;

        // 获得当前整个字符串的包围盒
        if (first) {
            minX = gx0;
            maxX = gx1;
            minY = y0;
            maxY = y1;
            first = false;
        } else {
            minX = std::min(minX, gx0);
            maxX = std::max(maxX, gx1);
            minY = std::min(minY, y0);
            maxY = std::max(maxY, y1);
        }

        // 更新画笔位置和"前一个字符"
        int advanceWidth = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&impl_->font, static_cast<int>(cp), &advanceWidth, &lsb);
        penX += kern + static_cast<int>(advanceWidth * scale);
        prev = cp;
    }

    // 未生成文字，返回空位图
    if (first) {
        return out;
    }

    // 根据文字宽高，设置位图的宽高以及分配空间
    const int textW = std::max(1, maxX - minX);
    const int textH = std::max(1, maxY - minY);

    out.width = static_cast<uint32_t>(textW + paddingPx * 2);
    out.height = static_cast<uint32_t>(textH + paddingPx * 2);
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

    // 第二次循环，填充位图数据
    penX = 0;
    prev = 0;
    Utf8Iterator itDraw(utf8Text);
    while (itDraw.hasNext()) {
        const uint32_t cp = itDraw.next();

        // 计算字符间距
        const int kern =
            (prev != 0)
                ? static_cast<int>(stbtt_GetCodepointKernAdvance(
                                       &impl_->font, static_cast<int>(prev), static_cast<int>(cp)) *
                                   scale)
                : 0;

        // 生成单个字符位图包围盒
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBox(&impl_->font, static_cast<int>(cp), scale, scale, &x0, &y0, &x1,
                                    &y1);

        // 生成单通到位图，0~255 完全透明 - 完全覆盖
        int gw = 0;
        int gh = 0;
        uint8_t *bmp = stbtt_GetCodepointBitmap(&impl_->font, scale, scale, static_cast<int>(cp), &gw,
                                                &gh, nullptr, nullptr);
        if (bmp) {
            // 将生成的位图写入到rgba中，位图对应的是通道alpha
            const int dstX0 = (penX + kern + x0 - minX) + paddingPx;
            const int dstY0 = (y0 - minY) + paddingPx;

            for (int y = 0; y < gh; ++y) {
                const int dy = dstY0 + y;
                if (dy < 0 || dy >= static_cast<int>(out.height)) {
                    continue;
                }
                for (int x = 0; x < gw; ++x) {
                    const int dx = dstX0 + x;
                    if (dx < 0 || dx >= static_cast<int>(out.width)) {
                        continue;
                    }

                    const uint8_t a = bmp[y * gw + x];
                    const size_t p =
                        (static_cast<size_t>(dy) * out.width + static_cast<uint32_t>(dx)) * 4;
                    out.rgba[p + 0] = 255;
                    out.rgba[p + 1] = 255;
                    out.rgba[p + 2] = 255;
                    out.rgba[p + 3] = a;
                }
            }

            // 释放生成的位图
            stbtt_FreeBitmap(bmp, nullptr);
        }

        // 移动画笔到下一个字符处
        int advanceWidth = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&impl_->font, static_cast<int>(cp), &advanceWidth, &lsb);
        penX += kern + static_cast<int>(advanceWidth * scale);
        prev = cp;
    }

    return out;
}
