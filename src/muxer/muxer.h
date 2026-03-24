#ifndef DECODER_SDK_INTERNAL_MUXER_H
#define DECODER_SDK_INTERNAL_MUXER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include "base/base_define.h"
#include "base/packet.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

/**
 * @brief 复用器（容器输出）封装
 *
 * 负责：
 * - 打开输出容器（文件/协议），创建 AVFormatContext
 * - 按编码器上下文添加流，写入文件头/包/文件尾
 * - 管理时间基（time_base）与线程安全
 *
 * 线程安全：
 * - 除 getStreamTimeBase 外，所有对内部 AVFormatContext 的访问均加锁保护
 * - open/writeHeader/writePacket/writeTrailer/close 需要在单线程顺序调用
 */
class Muxer {
public:
    Muxer();
    virtual ~Muxer();

    // Disable copy
    Muxer(const Muxer &) = delete;
    Muxer &operator=(const Muxer &) = delete;

    /**
     * @brief 打开复用器
     * @param url 输出URL
     * @param format 输出格式（如 "mp4", "flv"），为空则根据url自动猜测
     * @return 成功返回true，失败返回false
     */
    bool open(const std::string &url, const std::string &format);

    /**
     * @brief 关闭复用器
     */
    void close();

    /**
     * @brief 添加流
     * @param codecCtx 编码器上下文，用于复制参数
     * @return 返回流索引，失败返回-1
     */
    int addStream(AVCodecContext *codecCtx);

    /**
     * @brief 写入文件头
     * @return 成功返回true，失败返回false
     */
    bool writeHeader();

    /**
     * @brief 写入数据包
     * @param packet 数据包
     * @return 成功返回true，失败返回false
     */
    bool writePacket(Packet &packet);

    /**
     * @brief 写入文件尾
     * @return 成功返回true，失败返回false
     */
    bool writeTrailer();

    /**
     * @brief 检查是否已打开
     * @return true/false
     */
    bool isOpened() const;

    /**
     * @brief 获取流的时间基
     * @param streamIndex 流索引
     * @return 时间基；若索引非法则返回 {1,1000} 作为默认回退
     */
    AVRational getStreamTimeBase(int streamIndex);

    /**
     * @brief 获取内部 AVFormatContext 指针（只读）
     * @return AVFormatContext 指针，注意悬空指针
     */
    const AVFormatContext *const formatContext() const;

private:
    AVFormatContext *formatContext_ = nullptr;
    mutable std::mutex mutex_;
    std::atomic<bool> isOpened_{false};
    std::atomic<bool> isHeaderWritten_{false};
    std::string url_;
    std::vector<AVStream *> streams_; // 持有输出流指针，便于获取 time_base 等信息
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_MUXER_H
