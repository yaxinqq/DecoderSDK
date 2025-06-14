#ifndef DECODER_SDK_INTERNAL_PACKET_H
#define DECODER_SDK_INTERNAL_PACKET_H
extern "C" {
#include <libavcodec/avcodec.h>
}

#include "base_define.h"

DECODER_SDK_NAMESPACE_BEGIN
INTERNAL_NAMESPACE_BEGIN

class Packet {
public:
    /**
     * @brief 构造一个空的Packet对象
     */
    Packet();
    /**
     * @brief 从AVPacket指针构造Packet对象
     * @param pkt AVPacket指针
     */
    explicit Packet(AVPacket *pkt);
    /**
     * @brief 析构Packet对象，释放关联的AVPacket资源
     */
    ~Packet();

    /**
     * @brief 拷贝构造Packet对象
     * @param other 要拷贝的Packet对象
     */
    Packet(const Packet &other);
    /**
     * @brief 拷贝构造Packet对象
     * @param other 要拷贝的Packet对象
     */
    Packet &operator=(const Packet &other);

    /**
     * @brief 移动构造Packet对象
     * @param other 要移动的Packet对象
     */
    Packet(Packet &&other) noexcept;
    /**
     * @brief 移动赋值Packet对象
     * @param other 要移动的Packet对象
     */
    Packet &operator=(Packet &&other) noexcept;

    /**
     * @brief 获取AVPacket指针
     * @return AVPacket指针
     */
    AVPacket *get() const;
    /**
     * @brief 检查Packet对象是否有效
     * @return 如果有效返回true，否则返回false
     */
    bool isValid() const;
    /**
     * @brief 检查Packet对象是否为空
     * @return 如果为空返回true，否则返回false
     */
    bool isEmpty() const;

    /**
     * @brief 设置Packet对象的序列号
     * @param serial 序列号
     */
    void setSerial(int serial);
    /**
     * @brief 获取Packet对象的序列号
     * @return 序列号
     */
    int serial() const;

    // 属性透传
    /**
     * @brief 获取Packet对象的数据指针
     * @return 数据指针
     */
    uint8_t *data() const;
    /**
     * @brief 获取Packet对象的数据大小
     * @return 数据大小
     */
    int size() const;
    /**
     * @brief 获取Packet对象的PTS时间戳
     * @return PTS时间戳
     */
    int64_t pts() const;
    /**
     * @brief 获取Packet对象的DTS时间戳
     * @return DTS时间戳
     */
    int64_t dts() const;
    /**
     * @brief 获取Packet对象的持续时间
     * @return 持续时间
     */
    int64_t duration() const;
    /**
     * @brief 获取Packet对象的流索引
     * @return 流索引
     */
    int streamIndex() const;
    /**
     * @brief 获取Packet对象的标志位
     * @return 标志位
     */
    int flags() const;

    // 属性设置方法
    /**
     * @brief 设置Packet对象的PTS时间戳
     * @param pts PTS时间戳
     */
    void setPts(int64_t pts);
    /**
     * @brief 设置Packet对象的DTS时间戳
     * @param dts DTS时间戳
     */
    void setDts(int64_t dts);
    /**
     * @brief 设置Packet对象的持续时间
     * @param duration 持续时间
     */
    void setDuration(int64_t duration);
    /**
     * @brief 设置Packet对象的流索引
     * @param index 流索引
     */
    void setStreamIndex(int index);
    /**
     * @brief 设置Packet对象的标志位
     * @param flags 标志位
     */
    void setFlags(int flags);

    // 实用方法
    /**
     * @brief 释放Packet对象的引用但不销毁对象
     */
    void unref();
    /**
     * @brief 深拷贝Packet对象
     * @return 深拷贝后的Packet对象
     */
    Packet clone() const;

private:
    // AVPcaket数据
    AVPacket *packet_ = nullptr;
    // 序列号
    int serial_ = 0;
};

INTERNAL_NAMESPACE_END
DECODER_SDK_NAMESPACE_END

#endif // DECODER_SDK_INTERNAL_PACKET_H