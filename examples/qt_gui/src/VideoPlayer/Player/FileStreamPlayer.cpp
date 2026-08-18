#include "FileStreamPlayer.h"
#include "InternalUtils.h"
#include "StreamManager.h"

#include <QCoreApplication>
#include <QDebug>

FileStreamPlayer::FileStreamPlayer(QWidget *parent)
    : VideoPlayer(parent)
{
    connect(this, &VideoPlayer::fileStreamLoopEnded, this, &FileStreamPlayer::close);
}

FileStreamPlayer::~FileStreamPlayer()
{
    close();
}

QString FileStreamPlayer::streamUrl() const
{
    return streamUrl_;
}

void FileStreamPlayer::open(const QString &url)
{
    if (playerState() != Stream::PlayerState::Stop) {
        close();
    }

    // 设置播放器状态-开始播放
    setPlayerState(Stream::PlayerState::Start);

    decoder_sdk::DecoderConfig config;
    config.enableAutoReconnect = false;
    config.enableParseUserSEIData = false;
    config.enableJitterDetector = false;
    config.createHwContextCallback =
        std::bind(&utils::createHwContextCallback, std::placeholders::_1);
    config.freeHwContextCallback = std::bind(&utils::freeHwContextCallback,
                                             std::placeholders::_1, std::placeholders::_2);

    StreamManager::instance()->openStream(impl_, url, Stream::OpenMode::kExclusive, config);
    StreamManager::instance()->setLoopMode(impl_, decoder_sdk::LoopMode::kNone);

    streamUrl_ = url;
}

void FileStreamPlayer::close()
{
    // 设置播放器状态-停止播放
    setPlayerState(Stream::PlayerState::Stop);

    // 关流
    StreamManager::instance()->closeStream(impl_);

    streamUrl_.clear();
}

void FileStreamPlayer::pause()
{
    if ((playerState() != Stream::PlayerState::Start && playerState() != Stream::PlayerState::Playing && playerState() != Stream::PlayerState::Resume)) {
        return;
    }

    setPlayerState(Stream::PlayerState::Pause);

    StreamManager::instance()->pause(impl_);
}

void FileStreamPlayer::resume()
{
    if (playerState() != Stream::PlayerState::Pause) {
        return;
    }

    setPlayerState(Stream::PlayerState::Resume);

    StreamManager::instance()->resume(impl_);
}


bool FileStreamPlayer::seekTo(double pts)
{
    return StreamManager::instance()->seek(impl_, pts);
}

bool FileStreamPlayer::setSpeed(double speed)
{
    return StreamManager::instance()->setSpeed(impl_, speed);
}

void FileStreamPlayer::startRecoding(const QString &recodDir)
{
    qWarning() << QStringLiteral("[FileStreamPlayer] Not supported recording!");
}

void FileStreamPlayer::stopRecoding()
{
    qWarning() << QStringLiteral("[FileStreamPlayer] Not supported recording!");
}

bool FileStreamPlayer::isRecording() const
{
    return StreamManager::instance()->isRecoding(impl_);
}