#include "Demuxer.h"

Demuxer::Demuxer()
{
    videoPacketQueue_ = std::make_shared<PacketQueue>();
    audioPacketQueue_ = std::make_shared<PacketQueue>();   
}

Demuxer::~Demuxer()
{
    stop();
    close();
}

bool Demuxer::open(const std::string& url)
{
    if (avformat_open_input(&formatContext_, url.c_str(), nullptr, nullptr) != 0) {
        return false;
    }
    if (avformat_find_stream_info(formatContext_, nullptr) < 0) {
        return false;
    }

    int ret = av_find_best_stream(formatContext_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    videoStreamIndex_ = ret >= 0 ? ret : -1;

    ret = av_find_best_stream(formatContext_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    audioStreamIndex_ = ret >= 0 ? ret : -1;

    return true;
}

AVFormatContext* Demuxer::formatContext() const
{
    return formatContext_;
}

int Demuxer::streamIndex(AVMediaType mediaType) const
{
    switch(mediaType) {
        case AVMEDIA_TYPE_VIDEO:
            return videoStreamIndex_;
        case AVMEDIA_TYPE_AUDIO:
            return audioStreamIndex_;
        default:
            return -1;
    }
}

std::shared_ptr<PacketQueue> Demuxer::packetQueue(AVMediaType mediaType) const
{
    switch(mediaType) {
        case AVMEDIA_TYPE_VIDEO:
            return videoPacketQueue_;
        case AVMEDIA_TYPE_AUDIO:
            return audioPacketQueue_;
        default:
            return nullptr;
    }
}

bool Demuxer::hasVideo() const
{
    return videoStreamIndex_ >= 0 && formatContext_ != nullptr;
}
    
bool Demuxer::hasAudio() const
{
    return audioStreamIndex_ >= 0 && formatContext_ != nullptr;
}

void Demuxer::close() 
{
    if (formatContext_) {
        avformat_close_input(&formatContext_);
        formatContext_ = nullptr;
    }
}

void Demuxer::start()
{
    if (isRunning_.load()) {
        // Todo: log
        return;
    }

    if (!formatContext_) {
        // Todo: log
        return;
    }

    if (!videoPacketQueue_ || !audioPacketQueue_) {
        // Todo: log
        return;
    }

    // 启动队列
    videoPacketQueue_->start();
    audioPacketQueue_->start();

    isRunning_.store(true);
    thread_ = std::thread(&Demuxer::demuxLoop, this);
}

void Demuxer::stop()
{
    if (!isRunning_.load()) {
        // Todo: log
        return;
    }

    isRunning_.store(false);

    // 中止队列
    videoPacketQueue_->abort();
    audioPacketQueue_->abort();

    if (thread_.joinable()) {
        thread_.join();
    }
}

void Demuxer::demuxLoop() 
{
    AVPacket* pkt = av_packet_alloc();
    if (!pkt)
        return;
    
    while (isRunning_)
    {
        // 检查队列是否已满，如果已满则等待
        if ((videoStreamIndex_ >= 0 && videoPacketQueue_->isFull()) ||
            (audioStreamIndex_ >= 0 && audioPacketQueue_->isFull()))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        int ret = av_read_frame(formatContext_, pkt);
        if (ret < 0)
        {
            // 文件结束或错误
            if (ret == AVERROR_EOF || avio_feof(formatContext_->pb))
            {
                // 文件结束，发送空包表示结束
                av_packet_unref(pkt);
                
                if (videoStreamIndex_ >= 0)
                {
                    pkt->stream_index = videoStreamIndex_;
                    Packet endPacket(pkt);
                    endPacket.setSerial(videoPacketQueue_->serial());
                    videoPacketQueue_->push(endPacket);
                }
                
                if (audioStreamIndex_ >= 0)
                {
                    pkt->stream_index = audioStreamIndex_;
                    Packet endPacket(pkt);
                    endPacket.setSerial(audioPacketQueue_->serial());
                    audioPacketQueue_->push(endPacket);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            else if (ret != AVERROR(EAGAIN))
            {
                // 真正的错误
                break;
            }
            continue;
        }
        
        // 根据流类型分发数据包
        if (pkt->stream_index == videoStreamIndex_)
        {
            Packet packet(pkt);
            packet.setSerial(videoPacketQueue_->serial());
            videoPacketQueue_->push(packet);
        }
        else if (pkt->stream_index == audioStreamIndex_)
        {
            Packet packet(pkt);
            packet.setSerial(audioPacketQueue_->serial());
            audioPacketQueue_->push(packet);
        }
        
        av_packet_unref(pkt);
    }
    
    av_packet_free(&pkt);
}