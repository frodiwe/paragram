/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "pararam_media_signaling.h"

#include "api/scoped_refptr.h"

#include <functional>
#include <memory>
#include <string>

namespace rtc {
template <typename VideoFrameT>
class VideoSinkInterface;
} // namespace rtc

namespace webrtc {
class AudioDeviceModule;
class TaskQueueFactory;
class VideoFrame;
class VideoTrackSourceInterface;
} // namespace webrtc

namespace tgcalls {
class Threads;
class VideoCaptureInterface;
template <typename T>
class ThreadLocalObject;
} // namespace tgcalls

namespace Pararam::Media {

class EngineInternal;

struct EngineDescriptor {
	std::shared_ptr<Signaling> signaling;
	std::function<webrtc::scoped_refptr<webrtc::AudioDeviceModule>(
		webrtc::TaskQueueFactory*)> createAudioDeviceModule;
	std::string audioInputId;
	std::string audioOutputId;
	std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture;
	bool muted = false;
	std::string defaultVideoQuality; // "high", "medium", "low", "novideo".

	// All callbacks are invoked on the tgcalls media thread.
	std::function<void(bool connected)> connectionChanged;
	std::function<void(const std::vector<Participant> &list)> participants;
	std::function<void(int64_t userId, bool speaking)> speaking;
	std::function<void()> ended; // Could not start the media.
};

// Media of one pararam.io call: one PeerConnection to the media server
// that sends our audio and video and receives the mixed audio, and one
// receive-only PeerConnection for the video of each other participant.
// Thread safe, the work happens on the tgcalls media thread.
class Engine final {
public:
	explicit Engine(EngineDescriptor &&descriptor);
	~Engine();

	void setMuted(bool muted);
	void setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> capture);
	void videoCaptureUpdated(); // Camera / screen switched.
	void setVideoSource(
		std::function<webrtc::scoped_refptr<
			webrtc::VideoTrackSourceInterface>()> getSource,
		bool screen);

	// userId == 0 means the first other participant (for 1:1 calls).
	void setVideoSink(
		int64_t userId,
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink);
	void setVideoQuality(int64_t userId, std::string quality);

	void setAudioInputDevice(std::string id);
	void setAudioOutputDevice(std::string id);

	void stop(std::function<void()> done);

private:
	std::shared_ptr<tgcalls::Threads> _threads;
	std::unique_ptr<tgcalls::ThreadLocalObject<EngineInternal>> _internal;

};

} // namespace Pararam::Media
