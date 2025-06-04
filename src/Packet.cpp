#include "Packet.h"

Packet::Packet() : packet_(av_packet_alloc()), serial_(0)
{
}

Packet::Packet(AVPacket *pkt) : Packet()
{
    if (pkt) {
        av_packet_ref(packet_, pkt);
    }
}

Packet::~Packet()
{
    if (packet_) {
        av_packet_free(&packet_);
    }
}

Packet::Packet(const Packet &other)
    : packet_(av_packet_alloc()), serial_(other.serial_)
{
    if (other.packet_) {
        int ret = av_packet_ref(packet_, other.packet_);
        if (ret < 0) {
            av_packet_free(&packet_);
            packet_ = nullptr;
        }
    }
}

Packet &Packet::operator=(const Packet &other)
{
    if (this != &other) {
        // 先清理当前资源
        if (packet_) {
            av_packet_unref(packet_);
        } else {
            packet_ = av_packet_alloc();
        }

        // 复制新资源
        if (other.packet_) {
            av_packet_ref(packet_, other.packet_);
        }
        serial_ = other.serial_;
    }
    return *this;
}

Packet::Packet(Packet &&other) noexcept
    : packet_(other.packet_), serial_(other.serial_)
{
    other.packet_ = nullptr;
    other.serial_ = 0;
}

Packet &Packet::operator=(Packet &&other) noexcept
{
    if (this != &other) {
        if (packet_) {
            av_packet_free(&packet_);
        }
        packet_ = other.packet_;
        serial_ = other.serial_;
        other.packet_ = nullptr;
        other.serial_ = 0;
    }
    return *this;
}

AVPacket *Packet::get() const
{
    return packet_;
}

bool Packet::isValid() const
{
    return packet_ != nullptr;
}

bool Packet::isEmpty() const
{
    return !packet_ || !packet_->data || packet_->size == 0;
}

void Packet::setSerial(int serial)
{
    serial_ = serial;
}

int Packet::serial() const
{
    return serial_;
}

// AVPacket属性透传实现
uint8_t *Packet::data() const
{
    return packet_ ? packet_->data : nullptr;
}

int Packet::size() const
{
    return packet_ ? packet_->size : 0;
}

int64_t Packet::pts() const
{
    return packet_ ? packet_->pts : AV_NOPTS_VALUE;
}

int64_t Packet::dts() const
{
    return packet_ ? packet_->dts : AV_NOPTS_VALUE;
}

int64_t Packet::duration() const
{
    return packet_ ? packet_->duration : 0;
}

int Packet::streamIndex() const
{
    return packet_ ? packet_->stream_index : -1;
}

int Packet::flags() const
{
    return packet_ ? packet_->flags : 0;
}

void Packet::setPts(int64_t pts)
{
    if (packet_) {
        packet_->pts = pts;
    }
}

void Packet::setDts(int64_t dts)
{
    if (packet_) {
        packet_->dts = dts;
    }
}

void Packet::setDuration(int64_t duration)
{
    if (packet_) {
        packet_->duration = duration;
    }
}

void Packet::setStreamIndex(int index)
{
    if (packet_) {
        packet_->stream_index = index;
    }
}

void Packet::unref()
{
    if (packet_) {
        av_packet_unref(packet_);
    }
}

Packet Packet::clone() const
{
    return Packet(*this);
}