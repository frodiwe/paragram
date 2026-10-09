/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam_call_instance.h"

#include "pararam_media_engine.h"
#include "pararam_media_signaling.h"

#include "VideoCaptureInterface.h"

#include <mutex>
#include <utility>

namespace Pararam::Media {
namespace {

std::mutex PendingMutex;
std::shared_ptr<Signaling> Pending;

} // namespace

void SetPendingSignaling(std::shared_ptr<Signaling> signaling) {
	const auto lock = std::lock_guard(PendingMutex);
	Pending = std::move(signaling);
}

std::shared_ptr<Signaling> TakePendingSignaling() {
	const auto lock = std::lock_guard(PendingMutex);
	return std::exchange(Pending, nullptr);
}

CallInstance::CallInstance(tgcalls::Descriptor &&descriptor)
: _signaling(TakePendingSignaling())
, _alive(std::make_shared<std::atomic<bool>>(true)) {
	const auto stateUpdated = descriptor.stateUpdated;
	if (!_signaling) {
		if (stateUpdated) {
			stateUpdated(tgcalls::State::Failed);
		}
		return;
	}
	const auto signalBarsUpdated = descriptor.signalBarsUpdated;
	const auto remoteMediaStateUpdated = descriptor.remoteMediaStateUpdated;
	const auto selfId = _signaling->selfId();
	const auto alive = _alive;

	auto engine = EngineDescriptor{
		.signaling = _signaling,
		.createAudioDeviceModule = descriptor.createAudioDeviceModule,
		.audioInputId = descriptor.mediaDevicesConfig.audioInputId,
		.audioOutputId = descriptor.mediaDevicesConfig.audioOutputId,
		.videoCapture = descriptor.videoCapture,
		.defaultVideoQuality = "high",
	};
	engine.connectionChanged = [=](bool connected) {
		if (!*alive) {
			return;
		}
		if (stateUpdated) {
			stateUpdated(connected
				? tgcalls::State::Established
				: tgcalls::State::Reconnecting);
		}
		if (signalBarsUpdated) {
			signalBarsUpdated(connected ? 4 : 0);
		}
	};
	engine.participants = [=](const std::vector<Participant> &list) {
		if (!*alive || !remoteMediaStateUpdated) {
			return;
		}
		for (const auto &participant : list) {
			if (participant.id == selfId || participant.id == kChoirmasterId) {
				continue;
			}
			remoteMediaStateUpdated(
				(participant.audio
					? tgcalls::AudioState::Active
					: tgcalls::AudioState::Muted),
				(participant.video
					? tgcalls::VideoState::Active
					: tgcalls::VideoState::Inactive));
			break;
		}
	};
	engine.ended = [=] {
		if (*alive && stateUpdated) {
			stateUpdated(tgcalls::State::Failed);
		}
	};
	_engine = std::make_unique<Engine>(std::move(engine));
}

CallInstance::~CallInstance() {
	*_alive = false;
}

int CallInstance::GetConnectionMaxLayer() {
	return 92;
}

std::vector<std::string> CallInstance::GetVersions() {
	return { kCallVersion };
}

void CallInstance::setNetworkType(tgcalls::NetworkType networkType) {
}

void CallInstance::setMuteMicrophone(bool muteMicrophone) {
	if (_engine) {
		_engine->setMuted(muteMicrophone);
	}
}

void CallInstance::setAudioOutputGainControlEnabled(bool enabled) {
}

void CallInstance::setEchoCancellationStrength(int strength) {
}

bool CallInstance::supportsVideo() {
	return true;
}

void CallInstance::setIncomingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	if (_engine) {
		_engine->setVideoSink(0, std::move(sink));
	}
}

void CallInstance::setAudioInputDevice(std::string id) {
	if (_engine) {
		_engine->setAudioInputDevice(std::move(id));
	}
}

void CallInstance::setAudioOutputDevice(std::string id) {
	if (_engine) {
		_engine->setAudioOutputDevice(std::move(id));
	}
}

void CallInstance::setInputVolume(float level) {
}

void CallInstance::setOutputVolume(float level) {
}

void CallInstance::setAudioOutputDuckingEnabled(bool enabled) {
}

void CallInstance::setIsLowBatteryLevel(bool isLowBatteryLevel) {
}

std::string CallInstance::getLastError() {
	return {};
}

std::string CallInstance::getDebugInfo() {
	return {};
}

int64_t CallInstance::getPreferredRelayId() {
	return 0;
}

tgcalls::TrafficStats CallInstance::getTrafficStats() {
	return {};
}

tgcalls::PersistentState CallInstance::getPersistentState() {
	return {};
}

void CallInstance::receiveSignalingData(const std::vector<uint8_t> &data) {
}

void CallInstance::setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture) {
	if (_engine) {
		_engine->setVideoCapture(std::move(videoCapture));
	}
}

void CallInstance::sendVideoDeviceUpdated() {
	if (_engine) {
		_engine->videoCaptureUpdated();
	}
}

void CallInstance::setRequestedVideoAspect(float aspect) {
}

void CallInstance::stop(std::function<void(tgcalls::FinalState)> completion) {
	*_alive = false;
	if (_signaling) {
		_signaling->leave();
	}
	if (!_engine) {
		completion({});
		return;
	}
	_engine->stop([=] {
		completion({});
	});
}

} // namespace Pararam::Media

namespace tgcalls {

template <>
bool Register<Pararam::Media::CallInstance>() {
	return Meta::RegisterOne<Pararam::Media::CallInstance>();
}

} // namespace tgcalls
