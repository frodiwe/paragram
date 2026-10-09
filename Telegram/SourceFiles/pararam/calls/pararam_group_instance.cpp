/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam_group_instance.h"

#include "pararam_media_engine.h"
#include "pararam_media_signaling.h"

#include "api/units/time_delta.h"
#include "rtc_base/thread.h"

#include <map>
#include <random>
#include <set>

namespace Pararam::Media {
namespace {

constexpr auto kLevelsIntervalMs = 150;
constexpr auto kSpeakingLevel = 0.7f;

[[nodiscard]] std::string QualityName(
		tgcalls::VideoChannelDescription::Quality quality) {
	using Quality = tgcalls::VideoChannelDescription::Quality;
	switch (quality) {
	case Quality::Thumbnail: return "low";
	case Quality::Medium: return "medium";
	case Quality::Full: return "high";
	}
	return "low";
}

[[nodiscard]] uint32_t RandomOwnSource() {
	auto device = std::random_device();
	return 0x60000000U | (uint32_t(device()) & 0x1FFFFFFFU);
}

} // namespace

class GroupInstanceState final
	: public std::enable_shared_from_this<GroupInstanceState> {
public:
	explicit GroupInstanceState(tgcalls::GroupInstanceDescriptor &&descriptor)
	: threads(descriptor.threads
		? descriptor.threads
		: tgcalls::StaticThreads::getThreads())
	, networkStateUpdated(std::move(descriptor.networkStateUpdated))
	, signalBarsUpdated(std::move(descriptor.signalBarsUpdated))
	, audioLevelsUpdated(std::move(descriptor.audioLevelsUpdated))
	, createAudioDeviceModule(std::move(descriptor.createAudioDeviceModule))
	, inputDeviceId(descriptor.initialInputDeviceId)
	, outputDeviceId(descriptor.initialOutputDeviceId)
	, videoCapture(std::move(descriptor.videoCapture))
	, getVideoSource(std::move(descriptor.getVideoSource)) {
	}

	template <typename Callback>
	void post(Callback &&callback, int delayMs = 0) {
		auto task = [weak = weak_from_this(), callback = std::forward<
				Callback>(callback)]() mutable {
			if (const auto strong = weak.lock()) {
				if (!strong->stopped) {
					callback(strong.get());
				}
			}
		};
		if (delayMs > 0) {
			threads->getMediaThread()->PostDelayedTask(
				std::move(task),
				webrtc::TimeDelta::Millis(delayMs));
		} else {
			threads->getMediaThread()->PostTask(std::move(task));
		}
	}

	void start(std::shared_ptr<Signaling> signaling) {
		if (engine || !signaling) {
			if (!signaling && networkStateUpdated) {
				networkStateUpdated({ .isConnected = false });
			}
			return;
		}
		selfId = signaling->selfId();
		const auto weak = weak_from_this();
		auto descriptor = EngineDescriptor{
			.signaling = signaling,
			.createAudioDeviceModule = createAudioDeviceModule,
			.audioInputId = inputDeviceId,
			.audioOutputId = outputDeviceId,
			.videoCapture = videoCapture,
			.muted = muted,
		};
		descriptor.connectionChanged = [=](bool connected) {
			if (const auto strong = weak.lock()) {
				if (strong->networkStateUpdated) {
					strong->networkStateUpdated({
						.isConnected = connected,
					});
				}
				if (strong->signalBarsUpdated) {
					strong->signalBarsUpdated(connected ? 4 : 0);
				}
			}
		};
		descriptor.speaking = [=](int64_t userId, bool speaking) {
			if (const auto strong = weak.lock()) {
				strong->speakingChanged(userId, speaking);
			}
		};
		descriptor.ended = [=] {
			if (const auto strong = weak.lock()) {
				if (strong->networkStateUpdated) {
					strong->networkStateUpdated({ .isConnected = false });
				}
			}
		};
		engine = std::make_unique<Engine>(std::move(descriptor));
		if (getVideoSource && !videoCapture) {
			engine->setVideoSource(getVideoSource, false);
		}
		for (const auto &[userId, sink] : sinks) {
			engine->setVideoSink(userId, sink);
		}
		for (const auto &[userId, quality] : qualities) {
			engine->setVideoQuality(userId, quality);
		}
	}

	void speakingChanged(int64_t userId, bool speaking) {
		if (speaking) {
			if (speakingUsers.emplace(userId).second
				&& speakingUsers.size() == 1) {
				emitLevels();
			}
		} else if (speakingUsers.erase(userId)) {
			if (audioLevelsUpdated) {
				audioLevelsUpdated({ { { sourceFor(userId), {} } } });
			}
		}
	}

	void emitLevels() {
		if (speakingUsers.empty() || !audioLevelsUpdated) {
			return;
		}
		auto update = tgcalls::GroupLevelsUpdate();
		for (const auto userId : speakingUsers) {
			update.updates.push_back({
				.ssrc = sourceFor(userId),
				.value = {
					.level = kSpeakingLevel,
					.voice = true,
					.isMuted = (userId == selfId) && muted,
				},
			});
		}
		audioLevelsUpdated(update);
		post([](GroupInstanceState *that) {
			that->emitLevels();
		}, kLevelsIntervalMs);
	}

	[[nodiscard]] uint32_t sourceFor(int64_t userId) const {
		// The call UI treats ssrc 0 as our own.
		return (userId == selfId) ? 0 : AudioSourceFor(userId);
	}

	void setRequested(
			const std::vector<tgcalls::VideoChannelDescription> &channels) {
		auto now = std::map<int64_t, std::string>();
		for (const auto &channel : channels) {
			if (const auto userId = UserFromEndpoint(channel.endpointId)) {
				now[userId] = QualityName(channel.maxQuality);
			}
		}
		for (const auto &[userId, quality] : qualities) {
			if (!now.contains(userId)) {
				now[userId] = "novideo";
			}
		}
		for (const auto &[userId, quality] : now) {
			const auto i = qualities.find(userId);
			if (i != end(qualities) && i->second == quality) {
				continue;
			}
			qualities[userId] = quality;
			if (engine) {
				engine->setVideoQuality(userId, quality);
			}
		}
	}

	const std::shared_ptr<tgcalls::Threads> threads;
	std::function<void(tgcalls::GroupNetworkState)> networkStateUpdated;
	std::function<void(int)> signalBarsUpdated;
	std::function<void(tgcalls::GroupLevelsUpdate const &)> audioLevelsUpdated;
	std::function<webrtc::scoped_refptr<webrtc::AudioDeviceModule>(
		webrtc::TaskQueueFactory*)> createAudioDeviceModule;
	std::string inputDeviceId;
	std::string outputDeviceId;
	std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture;
	std::function<webrtc::scoped_refptr<
		webrtc::VideoTrackSourceInterface>()> getVideoSource;

	std::unique_ptr<Engine> engine;
	int64_t selfId = 0;
	uint32_t ownSource = RandomOwnSource();
	bool muted = true;
	bool stopped = false;
	std::map<int64_t, std::weak_ptr<
		rtc::VideoSinkInterface<webrtc::VideoFrame>>> sinks;
	std::map<int64_t, std::string> qualities;
	std::set<int64_t> speakingUsers;

};

GroupInstance::GroupInstance(tgcalls::GroupInstanceDescriptor &&descriptor)
: _state(std::make_shared<GroupInstanceState>(std::move(descriptor))) {
}

GroupInstance::~GroupInstance() {
	_state->post([](GroupInstanceState *that) {
		that->stopped = true;
		that->engine = nullptr;
	});
}

void GroupInstance::stop(std::function<void()> completion) {
	_state->post([=](GroupInstanceState *that) {
		that->stopped = true;
		if (const auto engine = that->engine.get()) {
			engine->stop(completion);
		} else if (completion) {
			completion();
		}
	});
}

void GroupInstance::setConnectionMode(
		tgcalls::GroupConnectionMode connectionMode,
		bool keepBroadcastIfWasEnabled,
		bool isUnifiedBroadcast) {
}

void GroupInstance::emitJoinPayload(
		std::function<void(tgcalls::GroupJoinPayload const &)> completion) {
	const auto source = _state->ownSource;
	completion({
		.audioSsrc = source,
		.json = "{\"pararam\":true,\"ssrc\":" + std::to_string(source) + "}",
	});
}

void GroupInstance::setJoinResponsePayload(std::string const &payload) {
	auto signaling = TakePendingSignaling();
	_state->post([=](GroupInstanceState *that) {
		that->start(signaling);
	});
}

void GroupInstance::removeSsrcs(std::vector<uint32_t> ssrcs) {
}

void GroupInstance::removeIncomingVideoSource(uint32_t ssrc) {
}

void GroupInstance::setIsMuted(bool isMuted) {
	_state->post([=](GroupInstanceState *that) {
		that->muted = isMuted;
		if (that->engine) {
			that->engine->setMuted(isMuted);
		}
	});
}

void GroupInstance::setIsNoiseSuppressionEnabled(
		bool isNoiseSuppressionEnabled) {
}

void GroupInstance::setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture) {
	_state->post([=](GroupInstanceState *that) {
		that->videoCapture = videoCapture;
		if (that->engine) {
			that->engine->setVideoCapture(videoCapture);
		}
	});
}

void GroupInstance::setVideoSource(
		std::function<webrtc::scoped_refptr<
			webrtc::VideoTrackSourceInterface>()> getVideoSource) {
	_state->post([=](GroupInstanceState *that) {
		that->getVideoSource = getVideoSource;
		if (that->engine) {
			that->engine->setVideoSource(getVideoSource, false);
		}
	});
}

void GroupInstance::setAudioOutputDevice(std::string id) {
	_state->post([=](GroupInstanceState *that) {
		that->outputDeviceId = id;
		if (that->engine) {
			that->engine->setAudioOutputDevice(id);
		}
	});
}

void GroupInstance::setAudioInputDevice(std::string id) {
	_state->post([=](GroupInstanceState *that) {
		that->inputDeviceId = id;
		if (that->engine) {
			that->engine->setAudioInputDevice(id);
		}
	});
}

void GroupInstance::addExternalAudioSamples(std::vector<uint8_t> &&samples) {
}

void GroupInstance::addOutgoingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	// The call UI renders its own camera preview from the capture.
}

void GroupInstance::addIncomingVideoOutput(
		std::string const &endpointId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	const auto userId = UserFromEndpoint(endpointId);
	if (!userId) {
		return;
	}
	_state->post([=](GroupInstanceState *that) {
		that->sinks[userId] = sink;
		if (that->engine) {
			that->engine->setVideoSink(userId, sink);
		}
	});
}

void GroupInstance::setVolume(uint32_t ssrc, double volume) {
}

void GroupInstance::setRequestedVideoChannels(
		std::vector<tgcalls::VideoChannelDescription> &&requestedVideoChannels) {
	_state->post([channels = std::move(requestedVideoChannels)](
			GroupInstanceState *that) {
		that->setRequested(channels);
	});
}

void GroupInstance::getStats(
		std::function<void(tgcalls::GroupInstanceStats)> completion) {
	completion({});
}

void GroupInstance::internal_addCustomNetworkEvent(bool isRemoteConnected) {
}

} // namespace Pararam::Media
