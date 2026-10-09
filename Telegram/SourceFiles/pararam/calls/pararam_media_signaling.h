/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Plain C++ interface between the pararam.io call signaling (Qt, in the
// app target) and the WebRTC media engine (compiled into lib_tgcalls).
namespace Pararam::Media {

// The media server pseudo user, see CallSession.
inline constexpr auto kChoirmasterId = int64_t(14548);

// Telegram group calls identify participants by audio "sources" and
// video "endpoints", pararam.io by user ids. Others get sources from
// [1, 0x20000000), their video from [0x20000000, 0x40000000), our own
// source is random in [0x60000000, 0x80000000).
[[nodiscard]] inline uint32_t AudioSourceFor(int64_t userId) {
	return uint32_t(userId % 0x1FFFFFFF) + 1;
}
[[nodiscard]] inline uint32_t VideoSourceFor(int64_t userId) {
	return AudioSourceFor(userId) + 0x20000000;
}
[[nodiscard]] inline std::string EndpointFor(int64_t userId) {
	return "u" + std::to_string(userId);
}
[[nodiscard]] inline int64_t UserFromEndpoint(const std::string &endpoint) {
	if (endpoint.size() < 2 || endpoint[0] != 'u') {
		return 0;
	}
	auto result = int64_t();
	for (auto i = size_t(1); i != endpoint.size(); ++i) {
		const auto c = endpoint[i];
		if (c < '0' || c > '9') {
			return 0;
		}
		result = result * 10 + (c - '0');
	}
	return result;
}

struct Participant {
	int64_t id = 0;
	bool audio = false;
	bool video = false;
	bool screen = false;
};

struct IceServer {
	std::string url;
	std::string username;
	std::string password;
};

struct IceConfig {
	std::vector<IceServer> servers;
	bool relayOnly = true;
};

// Implemented on top of the media server socket. Methods may be called
// from any thread, callbacks are invoked on an unspecified thread.
class Signaling {
public:
	struct Events {
		std::function<void(const std::vector<Participant> &list)> participants;
		std::function<void(int64_t userId, bool speaking)> speaking;
		std::function<void()> ended;
	};

	virtual ~Signaling() = default;

	[[nodiscard]] virtual IceConfig iceConfig() const = 0;
	[[nodiscard]] virtual int64_t selfId() const = 0;

	// The current state is delivered right away.
	virtual void setEvents(Events events) = 0;

	virtual void sendOffer(
		int64_t dstUserId,
		std::string sdp,
		bool iceRestart,
		std::function<void(std::optional<std::string> answer)> done) = 0;
	virtual void setAudioEnabled(bool enabled) = 0;
	virtual void setVideoEnabled(bool enabled, bool screen) = 0;
	virtual void requestVideoQualities(
		std::map<int64_t, std::string> qualities) = 0;
	virtual void leave() = 0;
};

// The backend puts the signaling of a call here right before it tells
// the call UI to start the media (phoneCall / joinGroupCall result),
// the media engine created by the UI then takes it.
void SetPendingSignaling(std::shared_ptr<Signaling> signaling);
[[nodiscard]] std::shared_ptr<Signaling> TakePendingSignaling();

} // namespace Pararam::Media
