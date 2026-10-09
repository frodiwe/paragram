/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <tgcalls/group/GroupInstanceImpl.h>

#include <atomic>
#include <memory>

namespace Pararam::Media {

class GroupInstanceState;

// Group call through the pararam.io media server, replaces
// tgcalls::GroupInstanceCustomImpl. The backend answers joinGroupCall
// once the media server accepted us and leaves the signaling for us.
class GroupInstance final : public tgcalls::GroupInstanceInterface {
public:
	explicit GroupInstance(tgcalls::GroupInstanceDescriptor &&descriptor);
	~GroupInstance();

	void stop(std::function<void()> completion) override;

	void setConnectionMode(
		tgcalls::GroupConnectionMode connectionMode,
		bool keepBroadcastIfWasEnabled,
		bool isUnifiedBroadcast) override;

	void emitJoinPayload(
		std::function<void(tgcalls::GroupJoinPayload const &)> completion)
		override;
	void setJoinResponsePayload(std::string const &payload) override;
	void removeSsrcs(std::vector<uint32_t> ssrcs) override;
	void removeIncomingVideoSource(uint32_t ssrc) override;

	void setIsMuted(bool isMuted) override;
	void setIsNoiseSuppressionEnabled(bool isNoiseSuppressionEnabled) override;
	void setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture)
		override;
	void setVideoSource(
		std::function<webrtc::scoped_refptr<
			webrtc::VideoTrackSourceInterface>()> getVideoSource) override;
	void setAudioOutputDevice(std::string id) override;
	void setAudioInputDevice(std::string id) override;
	void addExternalAudioSamples(std::vector<uint8_t> &&samples) override;

	void addOutgoingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink)
		override;
	void addIncomingVideoOutput(
		std::string const &endpointId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink)
		override;

	void setVolume(uint32_t ssrc, double volume) override;
	void setRequestedVideoChannels(
		std::vector<tgcalls::VideoChannelDescription> &&requestedVideoChannels)
		override;

	void getStats(
		std::function<void(tgcalls::GroupInstanceStats)> completion) override;
	void internal_addCustomNetworkEvent(bool isRemoteConnected) override;

private:
	std::shared_ptr<GroupInstanceState> _state;

};

} // namespace Pararam::Media
