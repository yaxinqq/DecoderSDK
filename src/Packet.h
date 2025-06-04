#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

class Packet {
public:
    Packet();
    explicit Packet(AVPacket *pkt);
    ~Packet();

    Packet(const Packet &other);
    Packet &operator=(const Packet &other);

    // 移动构造和移动赋值
    Packet(Packet &&other) noexcept;
    Packet &operator=(Packet &&other) noexcept;

    // 基础访问方法
    AVPacket *get() const;
    bool isValid() const;
    bool isEmpty() const;  // 检查是否为空包（EOF标记）
    
    // 序列号相关
    void setSerial(int serial);
    int serial() const;
    
    // AVPacket属性透传
    uint8_t *data() const;
    int size() const;
    int64_t pts() const;
    int64_t dts() const;
    int64_t duration() const;
    int streamIndex() const;
    int flags() const;
    
    // 属性设置方法
    void setPts(int64_t pts);
    void setDts(int64_t dts);
    void setDuration(int64_t duration);
    void setStreamIndex(int index);
    
    // 实用方法
    void unref();  // 释放引用但不销毁对象
    Packet clone() const;  // 深拷贝

private:
    AVPacket *packet_ = nullptr;
    int serial_ = 0;
};