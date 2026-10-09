/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <tgcalls/Instance.h>

#include <atomic>
#include <memory>

namespace Pararam::Media {

class Engine;
class Signaling;

// The protocol version the backend puts into phoneCall.protocol so that
// Calls::Call creates this implementation through tgcalls::Meta.
inline constexpr auto kCallVersion = "pararam.1";

// 1:1 call through the pararam.io media server.
class CallInstance final : public tgcalls::Instance {
public:
	explicit CallInstance(tgcalls::Descriptor &&descriptor);
	~CallInstance();

	static int GetConnectionMaxLayer();
	static std::vector<std::string> GetVersions();

	void setNetworkType(tgcalls::NetworkType networkType) override;
	void setMuteMicrophone(bool muteMicrophone) override;
	void setAudioOutputGainControlEnabled(bool enabled) override;
	void setEchoCancellationStrength(int strength) override;

	bool supportsVideo() override;
	void setIncomingVideoOutput(
		std::weak_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink)
		override;

	void setAudioInputDevice(std::string id) override;
	void setAudioOutputDevice(std::string id) override;
	void setInputVolume(float level) override;
	void setOutputVolume(float level) override;
	void setAudioOutputDuckingEnabled(bool enabled) override;
	void setIsLowBatteryLevel(bool isLowBatteryLevel) override;

	std::string getLastError() override;
	std::string getDebugInfo() override;
	int64_t getPreferredRelayId() override;
	tgcalls::TrafficStats getTrafficStats() override;
	tgcalls::PersistentState getPersistentState() override;

	void receiveSignalingData(const std::vector<uint8_t> &data) override;
	void setVideoCapture(
		std::shared_ptr<tgcalls::VideoCaptureInterface> videoCapture)
		override;
	void sendVideoDeviceUpdated() override;
	void setRequestedVideoAspect(float aspect) override;

	void stop(std::function<void(tgcalls::FinalState)> completion) override;

private:
	std::shared_ptr<Signaling> _signaling;
	std::unique_ptr<Engine> _engine;
	std::shared_ptr<std::atomic<bool>> _alive;

};

} // namespace Pararam::Media
