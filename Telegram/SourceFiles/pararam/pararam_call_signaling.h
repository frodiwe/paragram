/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "pararam/calls/pararam_media_signaling.h"

#include <QtCore/QPointer>

#include <mutex>

namespace Pararam {

class CallSession;

// Media::Signaling on top of CallSession, marshals everything
// to the main thread.
class SessionSignaling final
	: public Media::Signaling
	, public std::enable_shared_from_this<SessionSignaling> {
public:
	explicit SessionSignaling(not_null<CallSession*> session);
	~SessionSignaling();

	// Main thread, right after construction.
	void subscribe();

	Media::IceConfig iceConfig() const override;
	int64_t selfId() const override;
	void setEvents(Events events) override;
	void sendOffer(
		int64_t dstUserId,
		std::string sdp,
		bool iceRestart,
		std::function<void(std::optional<std::string> answer)> done) override;
	void setAudioEnabled(bool enabled) override;
	void setVideoEnabled(bool enabled, bool screen) override;
	void requestVideoQualities(
		std::map<int64_t, std::string> qualities) override;
	void leave() override;

private:
	[[nodiscard]] std::vector<Media::Participant> participants() const;
	void notifyParticipants();
	[[nodiscard]] Events events() const;

	QPointer<CallSession> _session;
	const int64_t _selfId = 0;
	Media::IceConfig _ice;
	int _listenerId = 0;

	mutable std::mutex _mutex;
	Events _events;

};

} // namespace Pararam
