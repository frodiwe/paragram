/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam_media_engine.h"

#include "AudioDeviceHelper.h"
#include "StaticThreads.h"
#include "ThreadLocalObject.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"

#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/enable_media.h"
#include "api/jsep.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video/video_frame.h"
#include "media/base/media_constants.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/logging.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include "api/make_ref_counted.h"
#include "api/units/time_delta.h"

#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace Pararam::Media {
namespace {

constexpr auto kRestartDelayMs = 5'000;
constexpr auto kDisconnectedRestartMs = 10'000;
constexpr auto kCameraBitrate = 800'000;
constexpr auto kScreenBitrate = 1'200'000;
constexpr auto kAudioBitrate = 32'000;
constexpr auto kOpusFmtp = "minptime=10;useinbandfec=1;maxplaybackrate=16000;"
	"maxaveragebitrate=24000;sprop-maxcapturerate=16000;usedtx=1";

// The pararam.io media server wants VP9, like the web client sends.
// A local mock media server (aiortc) can do only VP8 and H264.
[[nodiscard]] bool AnyVideoCodec() {
	const auto value = std::getenv("PARARAM_CALL_ANY_CODEC");
	return value && *value;
}

[[nodiscard]] tgcalls::VideoCaptureInterfaceObject *CaptureObject(
		tgcalls::VideoCaptureInterface *capture) {
	return capture
		? static_cast<tgcalls::VideoCaptureInterfaceImpl*>(
			capture)->object()->getSyncAssumingSameThread()
		: nullptr;
}

[[nodiscard]] std::vector<std::string> SplitLines(const std::string &sdp) {
	auto result = std::vector<std::string>();
	auto stream = std::istringstream(sdp);
	auto line = std::string();
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		result.push_back(line);
	}
	while (!result.empty() && result.back().empty()) {
		result.pop_back();
	}
	return result;
}

// The web client marks the media sections it sends to the media server
// and asks for a narrow band Opus, do the same.
[[nodiscard]] std::string MungeOffer(const std::string &sdp) {
	const auto lines = SplitLines(sdp);
	auto opusPayloadTypes = std::vector<std::string>();
	for (const auto &line : lines) {
		const auto prefix = std::string("a=rtpmap:");
		if (line.rfind(prefix, 0) == 0) {
			const auto space = line.find(' ');
			if (space != std::string::npos) {
				auto codec = line.substr(space + 1);
				for (auto &c : codec) {
					c = char(std::tolower(c));
				}
				if (codec.rfind("opus/", 0) == 0) {
					opusPayloadTypes.push_back(
						line.substr(prefix.size(), space - prefix.size()));
				}
			}
		}
	}
	const char *videoQualities[] = { "high", "medium", "low" };
	auto videoIndex = 0;
	auto result = std::string();
	for (const auto &line : lines) {
		auto replaced = false;
		for (const auto &pt : opusPayloadTypes) {
			const auto prefix = "a=fmtp:" + pt + " ";
			if (line.rfind(prefix, 0) == 0) {
				result += prefix + kOpusFmtp + "\r\n";
				replaced = true;
				break;
			}
		}
		if (replaced) {
			continue;
		}
		result += line + "\r\n";
		if (line.rfind("m=audio", 0) == 0) {
			result += "i=pararam-quality-audio:medium\r\n";
		} else if (line.rfind("m=video", 0) == 0) {
			result += std::string("i=pararam-quality-video:")
				+ videoQualities[std::min(videoIndex++, 2)]
				+ "\r\n";
		}
	}
	return result;
}

class SessionDescriptionObserver
	: public webrtc::CreateSessionDescriptionObserver {
public:
	using Done = std::function<void(
		std::unique_ptr<webrtc::SessionDescriptionInterface>)>;

	explicit SessionDescriptionObserver(Done done) : _done(std::move(done)) {
	}

	void OnSuccess(webrtc::SessionDescriptionInterface *desc) override {
		_done(std::unique_ptr<webrtc::SessionDescriptionInterface>(desc));
	}
	void OnFailure(webrtc::RTCError error) override {
		RTC_LOG(LS_ERROR) << "Pararam: CreateOffer failed: "
			<< error.message();
		_done(nullptr);
	}

private:
	Done _done;

};

class SetDescriptionObserver
	: public webrtc::SetLocalDescriptionObserverInterface
	, public webrtc::SetRemoteDescriptionObserverInterface {
public:
	using Done = std::function<void(webrtc::RTCError)>;

	explicit SetDescriptionObserver(Done done) : _done(std::move(done)) {
	}

	void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
		_done(std::move(error));
	}
	void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
		_done(std::move(error));
	}

private:
	Done _done;

};

class PeerObserver final : public webrtc::PeerConnectionObserver {
public:
	std::function<void(webrtc::PeerConnectionInterface::IceConnectionState)>
		iceStateChanged;
	std::function<void(
		webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>)> track;

	void OnSignalingChange(
		webrtc::PeerConnectionInterface::SignalingState) override {
	}
	void OnDataChannel(
		webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {
	}
	void OnRenegotiationNeeded() override {
	}
	void OnIceGatheringChange(
		webrtc::PeerConnectionInterface::IceGatheringState) override {
	}
	void OnIceCandidate(const webrtc::IceCandidateInterface *) override {
		// The media server learns our relay address from STUN checks,
		// like with the web client, which never sends candidates.
	}
	void OnIceConnectionChange(
			webrtc::PeerConnectionInterface::IceConnectionState state)
			override {
		if (iceStateChanged) {
			iceStateChanged(state);
		}
	}
	void OnTrack(
			webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
			override {
		if (track) {
			track(transceiver);
		}
	}

};

// Keeps the frames flowing to whatever sink the UI currently wants.
class VideoForwarder final : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
	void OnFrame(const webrtc::VideoFrame &frame) override {
		if (const auto strong = _sink.lock()) {
			strong->OnFrame(frame);
		}
	}
	void OnDiscardedFrame() override {
		if (const auto strong = _sink.lock()) {
			strong->OnDiscardedFrame();
		}
	}
	void setSink(
			std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
		_sink = std::move(sink);
	}

private:
	std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _sink;

};

} // namespace

class EngineInternal final
	: public std::enable_shared_from_this<EngineInternal> {
public:
	EngineInternal(
		EngineDescriptor &&descriptor,
		std::shared_ptr<tgcalls::Threads> threads);
	~EngineInternal();

	void start();
	void setMuted(bool muted);
	void setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> capture);
	void videoCaptureUpdated();
	void setVideoSource(
		std::function<webrtc::scoped_refptr<
			webrtc::VideoTrackSourceInterface>()> getSource,
		bool screen);
	void setVideoSink(
		int64_t userId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink);
	void setVideoQuality(int64_t userId, std::string quality);
	void setAudioInputDevice(std::string id);
	void setAudioOutputDevice(std::string id);
	void stop();

private:
	struct Peer {
		int64_t userId = 0;
		webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection;
		std::unique_ptr<PeerObserver> observer;
		webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> video;
		std::shared_ptr<VideoForwarder> forwarder;
		bool negotiating = false;
		bool restartQueued = false;
		bool connected = false;
		int64_t lastRestart = 0;
		int generation = 0;
	};

	[[nodiscard]] webrtc::scoped_refptr<webrtc::AudioDeviceModule> createAdm();
	[[nodiscard]] std::unique_ptr<Peer> createPeer(int64_t userId);
	void negotiate(int64_t userId, bool iceRestart);
	void applyAnswer(
		int64_t userId,
		int generation,
		std::optional<std::string> answer);
	void iceStateChanged(
		int64_t userId,
		webrtc::PeerConnectionInterface::IceConnectionState state);
	void participantsUpdated(const std::vector<Participant> &list);
	void attachSink(Peer &peer);
	void applyOutgoingVideo();
	[[nodiscard]] Peer *findPeer(int64_t userId);
	[[nodiscard]] Peer *defaultPeer();
	void setConnected(bool connected);

	template <typename Callback>
	void post(Callback &&callback, int delayMs = 0);

	const std::shared_ptr<tgcalls::Threads> _threads;
	EngineDescriptor _descriptor;
	std::unique_ptr<webrtc::TaskQueueFactory> _taskQueueFactory;
	webrtc::scoped_refptr<webrtc::AudioDeviceModule> _adm;
	webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> _factory;
	webrtc::scoped_refptr<webrtc::AudioTrackInterface> _audioTrack;
	webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> _audio;
	webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> _video;
	webrtc::scoped_refptr<webrtc::VideoTrackInterface> _videoTrack;
	std::shared_ptr<tgcalls::VideoCaptureInterface> _videoCapture;
	std::function<webrtc::scoped_refptr<
		webrtc::VideoTrackSourceInterface>()> _getVideoSource;
	bool _getVideoSourceScreen = false;
	bool _videoScreen = false;
	std::unique_ptr<Peer> _choirmaster;
	std::map<int64_t, std::unique_ptr<Peer>> _peers;
	std::map<int64_t, std::weak_ptr<
		rtc::VideoSinkInterface<webrtc::VideoFrame>>> _sinks;
	std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _defaultSink;
	std::map<int64_t, std::string> _qualities;
	bool _muted = false;
	bool _connected = false;
	bool _stopped = false;

};

EngineInternal::EngineInternal(
	EngineDescriptor &&descriptor,
	std::shared_ptr<tgcalls::Threads> threads)
: _threads(std::move(threads))
, _descriptor(std::move(descriptor))
, _taskQueueFactory(webrtc::CreateDefaultTaskQueueFactory())
, _videoCapture(_descriptor.videoCapture)
, _muted(_descriptor.muted) {
}

EngineInternal::~EngineInternal() {
	stop();
	_threads->getWorkerThread()->BlockingCall([&] {
		_adm = nullptr;
	});
}

template <typename Callback>
void EngineInternal::post(Callback &&callback, int delayMs) {
	const auto weak = std::weak_ptr<EngineInternal>(shared_from_this());
	auto task = [weak, callback = std::forward<Callback>(callback)]() mutable {
		if (const auto strong = weak.lock()) {
			if (!strong->_stopped) {
				callback(strong.get());
			}
		}
	};
	if (delayMs > 0) {
		_threads->getMediaThread()->PostDelayedTask(
			std::move(task),
			webrtc::TimeDelta::Millis(delayMs));
	} else {
		_threads->getMediaThread()->PostTask(std::move(task));
	}
}

webrtc::scoped_refptr<webrtc::AudioDeviceModule> EngineInternal::createAdm() {
	const auto check = [](
			const webrtc::scoped_refptr<webrtc::AudioDeviceModule> &result) {
		return (result && result->Init() == 0) ? result : nullptr;
	};
	if (_descriptor.createAudioDeviceModule) {
		if (const auto result = check(
				_descriptor.createAudioDeviceModule(
					_taskQueueFactory.get()))) {
			return result;
		}
	}
	return check(webrtc::AudioDeviceModule::Create(
		webrtc::AudioDeviceModule::kPlatformDefaultAudio,
		_taskQueueFactory.get()));
}

void EngineInternal::start() {
	tgcalls::PlatformInterface::SharedInstance()->configurePlatformAudio();

	_threads->getWorkerThread()->BlockingCall([&] {
		_adm = createAdm();
	});

	auto dependencies = webrtc::PeerConnectionFactoryDependencies();
	dependencies.network_thread = _threads->getNetworkThread();
	dependencies.signaling_thread = _threads->getMediaThread();
	dependencies.worker_thread = _threads->getWorkerThread();
	dependencies.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
	dependencies.network_monitor_factory = tgcalls::PlatformInterface::
		SharedInstance()->createNetworkMonitorFactory();
	dependencies.adm = _adm;
	dependencies.audio_processing = webrtc::AudioProcessingBuilder().Create();
	dependencies.audio_encoder_factory
		= webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
	dependencies.audio_decoder_factory
		= webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
	dependencies.video_encoder_factory = tgcalls::PlatformInterface::
		SharedInstance()->makeVideoEncoderFactory(true);
	dependencies.video_decoder_factory = tgcalls::PlatformInterface::
		SharedInstance()->makeVideoDecoderFactory();
	webrtc::EnableMedia(dependencies);
	dependencies.event_log_factory = std::make_unique<
		webrtc::RtcEventLogFactory>(dependencies.task_queue_factory.get());
	_factory = webrtc::CreateModularPeerConnectionFactory(
		std::move(dependencies));
	if (!_factory) {
		RTC_LOG(LS_ERROR) << "Pararam: Could not create the factory.";
		if (_descriptor.ended) {
			_descriptor.ended();
		}
		return;
	}

	// Only now, like tgcalls: choosing the output device starts the
	// playout, and the device refuses the audio callback of the factory
	// while it plays, leaving the call without sound both ways.
	_threads->getWorkerThread()->BlockingCall([&] {
		if (_adm) {
			tgcalls::SetAudioInputDeviceById(
				_adm.get(),
				_descriptor.audioInputId);
			tgcalls::SetAudioOutputDeviceById(
				_adm.get(),
				_descriptor.audioOutputId);
		}
	});

	auto options = cricket::AudioOptions();
	options.echo_cancellation = true;
	options.noise_suppression = true;
	options.auto_gain_control = true;
	const auto source = _factory->CreateAudioSource(options);
	_audioTrack = _factory->CreateAudioTrack("audio0", source.get());
	_audioTrack->set_enabled(!_muted);

	_choirmaster = createPeer(kChoirmasterId);
	if (!_choirmaster) {
		if (_descriptor.ended) {
			_descriptor.ended();
		}
		return;
	}
	if (_videoCapture) {
		applyOutgoingVideo();
	}
	negotiate(kChoirmasterId, false);

	const auto weak = std::weak_ptr<EngineInternal>(shared_from_this());
	const auto threads = _threads;
	const auto postToMedia = [=](auto callback) {
		threads->getMediaThread()->PostTask([=] {
			if (const auto strong = weak.lock()) {
				if (!strong->_stopped) {
					callback(strong.get());
				}
			}
		});
	};
	_descriptor.signaling->setEvents({
		.participants = [=](const std::vector<Participant> &list) {
			postToMedia([=](EngineInternal *that) {
				that->participantsUpdated(list);
			});
		},
		.speaking = [=](int64_t userId, bool speaking) {
			postToMedia([=](EngineInternal *that) {
				if (that->_descriptor.speaking) {
					that->_descriptor.speaking(userId, speaking);
				}
			});
		},
		// The end of the call itself is handled by the backend.
		.ended = nullptr,
	});
	if (!_muted) {
		_descriptor.signaling->setAudioEnabled(true);
	} else {
		_descriptor.signaling->setAudioEnabled(false);
	}
}

auto EngineInternal::createPeer(int64_t userId) -> std::unique_ptr<Peer> {
	const auto ice = _descriptor.signaling->iceConfig();
	auto configuration = webrtc::PeerConnectionInterface::RTCConfiguration();
	configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
	configuration.bundle_policy
		= webrtc::PeerConnectionInterface::kBundlePolicyBalanced;
	configuration.rtcp_mux_policy
		= webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
	configuration.type = ice.relayOnly
		? webrtc::PeerConnectionInterface::IceTransportsType::kRelay
		: webrtc::PeerConnectionInterface::IceTransportsType::kAll;
	configuration.continual_gathering_policy = webrtc::
		PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY;
	for (const auto &server : ice.servers) {
		auto mapped = webrtc::PeerConnectionInterface::IceServer();
		mapped.urls.push_back(server.url);
		mapped.username = server.username;
		mapped.password = server.password;
		configuration.servers.push_back(std::move(mapped));
	}

	auto peer = std::make_unique<Peer>();
	peer->userId = userId;
	peer->observer = std::make_unique<PeerObserver>();
	peer->forwarder = std::make_shared<VideoForwarder>();

	const auto weak = std::weak_ptr<EngineInternal>(shared_from_this());
	const auto threads = _threads;
	peer->observer->iceStateChanged = [=](
			webrtc::PeerConnectionInterface::IceConnectionState state) {
		threads->getMediaThread()->PostTask([=] {
			if (const auto strong = weak.lock()) {
				if (!strong->_stopped) {
					strong->iceStateChanged(userId, state);
				}
			}
		});
	};
	peer->observer->track = [=](
			webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
		threads->getMediaThread()->PostTask([=] {
			const auto strong = weak.lock();
			if (!strong || strong->_stopped) {
				return;
			}
			const auto peer = strong->findPeer(userId);
			if (!peer || !transceiver->receiver()) {
				return;
			}
			const auto track = transceiver->receiver()->track();
			if (!track) {
				return;
			}
			if (transceiver->media_type() == cricket::MEDIA_TYPE_AUDIO) {
				// Only the media server mix is played, like on the web.
				track->set_enabled(userId == kChoirmasterId);
			} else if (transceiver->media_type()
					== cricket::MEDIA_TYPE_VIDEO
				&& userId != kChoirmasterId) {
				const auto video = static_cast<webrtc::VideoTrackInterface*>(
					track.get());
				video->AddOrUpdateSink(
					peer->forwarder.get(),
					rtc::VideoSinkWants());
			}
		});
	};

	auto dependencies = webrtc::PeerConnectionDependencies(
		peer->observer.get());
	auto result = _factory->CreatePeerConnectionOrError(
		configuration,
		std::move(dependencies));
	if (!result.ok()) {
		RTC_LOG(LS_ERROR) << "Pararam: Could not create a connection: "
			<< result.error().message();
		return nullptr;
	}
	peer->connection = result.MoveValue();

	const auto vp9Only = [&](bool sender) {
		auto result = std::vector<webrtc::RtpCodecCapability>();
		if (AnyVideoCodec()) {
			return result;
		}
		const auto capabilities = sender
			? _factory->GetRtpSenderCapabilities(cricket::MEDIA_TYPE_VIDEO)
			: _factory->GetRtpReceiverCapabilities(cricket::MEDIA_TYPE_VIDEO);
		for (const auto &codec : capabilities.codecs) {
			if (codec.name == cricket::kVp9CodecName) {
				result.push_back(codec);
			}
		}
		return result;
	};

	if (userId == kChoirmasterId) {
		auto audioInit = webrtc::RtpTransceiverInit();
		audioInit.direction = webrtc::RtpTransceiverDirection::kSendRecv;
		audioInit.stream_ids = { "stream0" };
		auto audio = peer->connection->AddTransceiver(_audioTrack, audioInit);
		if (audio.ok()) {
			_audio = audio.MoveValue();
			auto parameters = _audio->sender()->GetParameters();
			if (parameters.encodings.empty()) {
				parameters.encodings.emplace_back();
			}
			parameters.encodings[0].max_bitrate_bps = kAudioBitrate;
			_audio->sender()->SetParameters(parameters);
		}

		auto videoInit = webrtc::RtpTransceiverInit();
		videoInit.direction = webrtc::RtpTransceiverDirection::kSendRecv;
		videoInit.stream_ids = { "stream0" };
		auto encoding = webrtc::RtpEncodingParameters();
		encoding.max_bitrate_bps = kCameraBitrate;
		videoInit.send_encodings = { encoding };
		auto video = peer->connection->AddTransceiver(
			cricket::MEDIA_TYPE_VIDEO,
			videoInit);
		if (video.ok()) {
			_video = video.MoveValue();
			auto codecs = vp9Only(true);
			if (!codecs.empty()) {
				_video->SetCodecPreferences(codecs);
			}
		}
	} else {
		auto audioInit = webrtc::RtpTransceiverInit();
		audioInit.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
		peer->connection->AddTransceiver(cricket::MEDIA_TYPE_AUDIO, audioInit);

		auto videoInit = webrtc::RtpTransceiverInit();
		videoInit.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
		auto video = peer->connection->AddTransceiver(
			cricket::MEDIA_TYPE_VIDEO,
			videoInit);
		if (video.ok()) {
			peer->video = video.MoveValue();
			auto codecs = vp9Only(false);
			if (!codecs.empty()) {
				peer->video->SetCodecPreferences(codecs);
			}
		}
		attachSink(*peer);
	}
	return peer;
}

void EngineInternal::negotiate(int64_t userId, bool iceRestart) {
	const auto peer = findPeer(userId);
	if (!peer || !peer->connection) {
		return;
	} else if (peer->negotiating) {
		peer->restartQueued = peer->restartQueued || iceRestart;
		return;
	}
	peer->negotiating = true;
	const auto generation = ++peer->generation;
	if (iceRestart) {
		peer->lastRestart = rtc::TimeMillis();
	}

	auto options = webrtc::PeerConnectionInterface::RTCOfferAnswerOptions();
	options.voice_activity_detection = true;
	options.ice_restart = iceRestart;

	const auto weak = std::weak_ptr<EngineInternal>(shared_from_this());
	const auto threads = _threads;
	const auto signaling = _descriptor.signaling;
	const auto failed = [=] {
		threads->getMediaThread()->PostTask([=] {
			if (const auto strong = weak.lock()) {
				strong->applyAnswer(userId, generation, std::nullopt);
			}
		});
	};
	const auto created = [=](
			std::unique_ptr<webrtc::SessionDescriptionInterface> desc) {
		threads->getMediaThread()->PostTask([=, desc = std::shared_ptr<
				webrtc::SessionDescriptionInterface>(std::move(desc))]() mutable {
			const auto strong = weak.lock();
			if (!strong || strong->_stopped) {
				return;
			}
			const auto peer = strong->findPeer(userId);
			if (!peer || peer->generation != generation) {
				return;
			} else if (!desc) {
				failed();
				return;
			}
			auto sdp = std::string();
			desc->ToString(&sdp);
			auto local = webrtc::CreateSessionDescription(
				webrtc::SdpType::kOffer,
				sdp);
			if (!local) {
				failed();
				return;
			}
			const auto observer = rtc::make_ref_counted<
				SetDescriptionObserver>([=](webrtc::RTCError error) {
				if (!error.ok()) {
					RTC_LOG(LS_ERROR) << "Pararam: SetLocalDescription failed: "
						<< error.message();
					failed();
					return;
				}
				// Sent right away, without waiting for ICE gathering,
				// like the web client does.
				const auto send = (userId == kChoirmasterId)
					? MungeOffer(sdp)
					: sdp;
				signaling->sendOffer(userId, send, iceRestart, [=](
						std::optional<std::string> answer) {
					threads->getMediaThread()->PostTask([=] {
						if (const auto strong = weak.lock()) {
							strong->applyAnswer(userId, generation, answer);
						}
					});
				});
			});
			peer->connection->SetLocalDescription(std::move(local), observer);
		});
	};
	peer->connection->CreateOffer(
		rtc::make_ref_counted<SessionDescriptionObserver>(created).get(),
		options);
}

void EngineInternal::applyAnswer(
		int64_t userId,
		int generation,
		std::optional<std::string> answer) {
	if (_stopped) {
		return;
	}
	const auto peer = findPeer(userId);
	if (!peer || peer->generation != generation) {
		return;
	}
	const auto finishNegotiation = [=] {
		const auto peer = findPeer(userId);
		if (!peer) {
			return;
		}
		peer->negotiating = false;
		if (std::exchange(peer->restartQueued, false)) {
			negotiate(userId, true);
		}
	};
	if (!answer) {
		RTC_LOG(LS_ERROR) << "Pararam: No answer for user " << userId;
		peer->negotiating = false;
		peer->restartQueued = false;
		// Try again a bit later.
		post([=](EngineInternal *that) {
			that->negotiate(userId, true);
		}, kRestartDelayMs);
		return;
	}
	auto error = webrtc::SdpParseError();
	auto remote = webrtc::CreateSessionDescription(
		webrtc::SdpType::kAnswer,
		*answer,
		&error);
	if (!remote) {
		RTC_LOG(LS_ERROR) << "Pararam: Bad answer: " << error.description;
		finishNegotiation();
		return;
	}
	const auto weak = std::weak_ptr<EngineInternal>(shared_from_this());
	const auto threads = _threads;
	const auto observer = rtc::make_ref_counted<SetDescriptionObserver>(
		[=](webrtc::RTCError error) {
		if (!error.ok()) {
			RTC_LOG(LS_ERROR) << "Pararam: SetRemoteDescription failed: "
				<< error.message();
		}
		threads->getMediaThread()->PostTask([=] {
			if (const auto strong = weak.lock()) {
				if (!strong->_stopped) {
					finishNegotiation();
				}
			}
		});
	});
	peer->connection->SetRemoteDescription(std::move(remote), observer);
}

void EngineInternal::iceStateChanged(
		int64_t userId,
		webrtc::PeerConnectionInterface::IceConnectionState state) {
	using State = webrtc::PeerConnectionInterface::IceConnectionState;
	const auto peer = findPeer(userId);
	if (!peer) {
		return;
	}
	const auto connected = (state == State::kIceConnectionConnected)
		|| (state == State::kIceConnectionCompleted);
	peer->connected = connected;
	if (userId == kChoirmasterId) {
		setConnected(connected);
	}
	const auto restartLater = [=](int delayMs) {
		const auto generation = peer->generation;
		post([=](EngineInternal *that) {
			const auto peer = that->findPeer(userId);
			if (peer && !peer->connected && peer->generation == generation) {
				that->negotiate(userId, true);
			}
		}, delayMs);
	};
	if (state == State::kIceConnectionFailed) {
		const auto since = rtc::TimeMillis() - peer->lastRestart;
		restartLater((since >= kRestartDelayMs)
			? 0
			: int(kRestartDelayMs - since));
	} else if (state == State::kIceConnectionDisconnected) {
		restartLater(kDisconnectedRestartMs);
	}
}

void EngineInternal::setConnected(bool connected) {
	if (_connected == connected) {
		return;
	}
	_connected = connected;
	if (_descriptor.connectionChanged) {
		_descriptor.connectionChanged(connected);
	}
}

void EngineInternal::participantsUpdated(
		const std::vector<Participant> &list) {
	auto alive = std::set<int64_t>();
	for (const auto &participant : list) {
		const auto id = participant.id;
		if (!id
			|| id == kChoirmasterId
			|| id == _descriptor.signaling->selfId()) {
			continue;
		}
		alive.emplace(id);
		if (!_peers.contains(id)) {
			if (auto peer = createPeer(id)) {
				_peers.emplace(id, std::move(peer));
				negotiate(id, false);
				if (!_descriptor.defaultVideoQuality.empty()
					&& !_qualities.contains(id)) {
					setVideoQuality(id, _descriptor.defaultVideoQuality);
				}
			}
		}
	}
	for (auto i = begin(_peers); i != end(_peers);) {
		if (!alive.contains(i->first)) {
			i->second->connection->Close();
			i = _peers.erase(i);
		} else {
			++i;
		}
	}
	for (auto &[id, peer] : _peers) {
		attachSink(*peer);
	}
	if (_descriptor.participants) {
		_descriptor.participants(list);
	}
}

void EngineInternal::attachSink(Peer &peer) {
	const auto i = _sinks.find(peer.userId);
	if (i != end(_sinks)) {
		peer.forwarder->setSink(i->second);
	} else if (defaultPeer() == &peer) {
		peer.forwarder->setSink(_defaultSink);
	} else {
		peer.forwarder->setSink({});
	}
}

auto EngineInternal::findPeer(int64_t userId) -> Peer* {
	if (userId == kChoirmasterId) {
		return _choirmaster.get();
	}
	const auto i = _peers.find(userId);
	return (i != end(_peers)) ? i->second.get() : nullptr;
}

auto EngineInternal::defaultPeer() -> Peer* {
	return _peers.empty() ? nullptr : _peers.begin()->second.get();
}

void EngineInternal::setMuted(bool muted) {
	if (_muted == muted) {
		return;
	}
	_muted = muted;
	if (_audioTrack) {
		_audioTrack->set_enabled(!muted);
	}
	_descriptor.signaling->setAudioEnabled(!muted);
}

void EngineInternal::setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> capture) {
	_videoCapture = std::move(capture);
	applyOutgoingVideo();
}

void EngineInternal::videoCaptureUpdated() {
	applyOutgoingVideo();
}

void EngineInternal::setVideoSource(
		std::function<webrtc::scoped_refptr<
			webrtc::VideoTrackSourceInterface>()> getSource,
		bool screen) {
	_getVideoSource = std::move(getSource);
	_getVideoSourceScreen = screen;
	applyOutgoingVideo();
}

void EngineInternal::applyOutgoingVideo() {
	if (!_video || !_factory) {
		return;
	}
	const auto object = CaptureObject(_videoCapture.get());
	auto source = object
		? object->source()
		: _getVideoSource
		? _getVideoSource()
		: webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>();
	if (!source) {
		if (_videoTrack) {
			_video->sender()->SetTrack(nullptr);
			_videoTrack = nullptr;
			_descriptor.signaling->setVideoEnabled(false, false);
		}
		return;
	}
	const auto screen = object
		? object->isScreenCapture()
		: _getVideoSourceScreen;
	_videoTrack = _factory->CreateVideoTrack(source, "video0");
	_video->sender()->SetTrack(_videoTrack.get());

	auto parameters = _video->sender()->GetParameters();
	if (parameters.encodings.empty()) {
		parameters.encodings.emplace_back();
	}
	auto &encoding = parameters.encodings[0];
	encoding.max_bitrate_bps = screen ? kScreenBitrate : kCameraBitrate;
	encoding.scalability_mode = screen
		? absl::optional<std::string>()
		: absl::optional<std::string>("L3T2_KEY");
	parameters.degradation_preference = screen
		? webrtc::DegradationPreference::MAINTAIN_RESOLUTION
		: webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
	const auto result = _video->sender()->SetParameters(parameters);
	if (!result.ok()) {
		// The negotiated codec may not support this scalability mode.
		encoding.scalability_mode = absl::nullopt;
		_video->sender()->SetParameters(parameters);
	}
	_videoScreen = screen;
	_descriptor.signaling->setVideoEnabled(true, screen);
}

void EngineInternal::setVideoSink(
		int64_t userId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	if (userId) {
		_sinks[userId] = std::move(sink);
	} else {
		_defaultSink = std::move(sink);
	}
	for (auto &[id, peer] : _peers) {
		attachSink(*peer);
	}
}

void EngineInternal::setVideoQuality(int64_t userId, std::string quality) {
	_qualities[userId] = quality;
	_descriptor.signaling->requestVideoQualities({ { userId, quality } });
}

void EngineInternal::setAudioInputDevice(std::string id) {
	const auto adm = _adm;
	_threads->getWorkerThread()->BlockingCall([&] {
		if (adm) {
			tgcalls::SetAudioInputDeviceById(adm.get(), id);
		}
	});
}

void EngineInternal::setAudioOutputDevice(std::string id) {
	const auto adm = _adm;
	_threads->getWorkerThread()->BlockingCall([&] {
		if (adm) {
			tgcalls::SetAudioOutputDeviceById(adm.get(), id);
		}
	});
}

void EngineInternal::stop() {
	if (_stopped) {
		return;
	}
	_stopped = true;
	if (_descriptor.signaling) {
		_descriptor.signaling->setEvents({});
	}
	for (auto &[id, peer] : _peers) {
		peer->connection->Close();
	}
	_peers.clear();
	if (_choirmaster) {
		_choirmaster->connection->Close();
		_choirmaster = nullptr;
	}
	_audio = nullptr;
	_video = nullptr;
	_videoTrack = nullptr;
	_audioTrack = nullptr;
	_factory = nullptr;
}

Engine::Engine(EngineDescriptor &&descriptor)
: _threads(tgcalls::StaticThreads::getThreads()) {
	_internal = std::make_unique<tgcalls::ThreadLocalObject<EngineInternal>>(
		_threads->getMediaThread(),
		[descriptor = std::move(descriptor), threads = _threads]() mutable {
			return std::make_shared<EngineInternal>(
				std::move(descriptor),
				threads);
		});
	_internal->perform([](EngineInternal *internal) {
		internal->start();
	});
}

Engine::~Engine() = default;

void Engine::setMuted(bool muted) {
	_internal->perform([=](EngineInternal *internal) {
		internal->setMuted(muted);
	});
}

void Engine::setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> capture) {
	_internal->perform([=](EngineInternal *internal) {
		internal->setVideoCapture(capture);
	});
}

void Engine::videoCaptureUpdated() {
	_internal->perform([](EngineInternal *internal) {
		internal->videoCaptureUpdated();
	});
}

void Engine::setVideoSource(
		std::function<webrtc::scoped_refptr<
			webrtc::VideoTrackSourceInterface>()> getSource,
		bool screen) {
	_internal->perform([=](EngineInternal *internal) {
		internal->setVideoSource(getSource, screen);
	});
}

void Engine::setVideoSink(
		int64_t userId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_internal->perform([=](EngineInternal *internal) {
		internal->setVideoSink(userId, sink);
	});
}

void Engine::setVideoQuality(int64_t userId, std::string quality) {
	_internal->perform([=](EngineInternal *internal) {
		internal->setVideoQuality(userId, quality);
	});
}

void Engine::setAudioInputDevice(std::string id) {
	_internal->perform([=](EngineInternal *internal) {
		internal->setAudioInputDevice(id);
	});
}

void Engine::setAudioOutputDevice(std::string id) {
	_internal->perform([=](EngineInternal *internal) {
		internal->setAudioOutputDevice(id);
	});
}

void Engine::stop(std::function<void()> done) {
	_internal->perform([=](EngineInternal *internal) {
		internal->stop();
		if (done) {
			done();
		}
	});
}

} // namespace Pararam::Media
