#include "RtspStreamPlayer.h"
#include "StreamManager.h"

#include <QCoreApplication>
#include <QDebug>

RtspStreamPlayer::RtspStreamPlayer(QWidget *parent)
	: VideoPlayer(parent)
{
	connect(this, &RtspStreamPlayer::streamClosed, this, &RtspStreamPlayer::onStreamClosed);
}

RtspStreamPlayer::~RtspStreamPlayer()
{
	close();
}

void RtspStreamPlayer::open(const QString &url, const QString &deviceId, const QString &channelId, Stream::OpenMode openMode)
{
	if (playerState() != Stream::PlayerState::Stop) { 
		close();  
	} 

	// 设置播放器状态-开始播放
	setPlayerState(Stream::PlayerState::Start);

	const auto decoderKey = StreamManager::instance()->openStream(impl_, url, deviceId, openMode);

	deviceId_ = deviceId;
	channelId_ = channelId;
	streamUrl_ = url;
	openMode_ = openMode;
	urlToDecoderKey_.insert(url, decoderKey);
}

void RtspStreamPlayer::close()
{
	// 设置播放器状态-停止播放
	setPlayerState(Stream::PlayerState::Stop);

	// 关流
	closeStream();

	streamUrl_.clear();
	deviceId_.clear();
	channelId_.clear();
	openMode_ = Stream::OpenMode::kExclusive;
	urlToDecoderKey_.clear();
}

void RtspStreamPlayer::pause()
{
	if ((playerState() != Stream::PlayerState::Start && playerState() != Stream::PlayerState::Playing && playerState() != Stream::PlayerState::Resume)) {
		return;
	}

	setPlayerState(Stream::PlayerState::Pause);

	StreamManager::instance()->pause(impl_);
}

void RtspStreamPlayer::resume()
{
	if (playerState() != Stream::PlayerState::Pause) {
		return;
	}

	setPlayerState(Stream::PlayerState::Resume);

	StreamManager::instance()->resume(impl_);
}

void RtspStreamPlayer::startRecoding(const QString &recoderPath)
{
    StreamManager::instance()->startRecoding(impl_, recoderPath);
}

void RtspStreamPlayer::stopRecoding()
{
    StreamManager::instance()->stopRecoding(impl_);
}

bool RtspStreamPlayer::isRecording() const
{
    return StreamManager::instance()->isRecoding(impl_);
}

bool RtspStreamPlayer::seek(double pts)
{
    return StreamManager::instance()->seek(impl_, pts);
}

bool RtspStreamPlayer::setSpeed(double speed)
{
    return StreamManager::instance()->setSpeed(impl_, speed);
}

bool RtspStreamPlayer::switchStream(const QString &url)
{
	// 如果当前没执行过open函数，则返回
	if (deviceId_.isEmpty())
		return false;

	// 如果url和当前正在播放的url是同一个，则返回true
	if (url == streamUrl_)
		return true;

	auto *const streamManager = StreamManager::instance();

	// 查询当前的url是否创建了解码器，如果没创建，则先创建
	if (!streamManager->isStreamExist(urlToDecoderKey_.value(url))) {
		const auto key = streamManager->createStream(url, openMode_);
		urlToDecoderKey_.insert(url, key);
	}

	// 设置播放器状态-停止播放
	setPlayerState(Stream::PlayerState::Stop);
	// 切换播放器
	streamManager->switchStream(urlToDecoderKey_.value(url), impl_);
	// 设置播放器状态-开始播放
	setPlayerState(Stream::PlayerState::Start);

	// 更新当前播放的地址
	streamUrl_ = url;
	return true;
}

void RtspStreamPlayer::onStreamClosed()
{
	// 目前仅输出，重连的逻辑由上层处理
	qWarning() << QStringLiteral("%1的连接已超时").arg(streamUrl_);
}

void RtspStreamPlayer::closeStream()
{
	// 关闭播放器的当前流
	StreamManager::instance()->closeStream(impl_);
	// 关闭播放器的补充流
	for (const auto &url : urlToDecoderKey_) {
		if (url == streamUrl_ || streamUrl_.isEmpty())
			continue;

		StreamManager::instance()->closeSupplementaryStream(urlToDecoderKey_.value(url), impl_);
	}
}