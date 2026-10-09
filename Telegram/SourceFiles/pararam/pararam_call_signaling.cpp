/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_call_signaling.h"

#include "pararam/pararam_call_session.h"

#include <crl/crl_on_main.h>

namespace Pararam {
namespace {

[[nodiscard]] Media::IceConfig ComputeIceConfig(const CallAccess &access) {
	auto result = Media::IceConfig();
	const auto host = access.turnHost.toStdString();
	const auto user = access.turnUser.toStdString();
	const auto password = access.turnPassword.toStdString();
	if (!host.empty()) {
		// The web client uses TURN over TLS on 5349, libwebrtc supports
		// only TCP for "turns", add plain TURN as a fallback.
		result.servers.push_back({
			"turns:" + host + ":5349?transport=tcp",
			user,
			password,
		});
		result.servers.push_back({
			"turn:" + host + ":3478?transport=udp",
			user,
			password,
		});
		result.servers.push_back({
			"turn:" + host + ":3478?transport=tcp",
			user,
			password,
		});
	}
	// For a local mock media server only.
	result.relayOnly = qEnvironmentVariableIsEmpty("PARARAM_CALL_ALLOW_DIRECT");
	return result;
}

} // namespace

SessionSignaling::SessionSignaling(not_null<CallSession*> session)
: _session(session.get())
, _selfId(session->selfId())
, _ice(ComputeIceConfig(session->access())) {
}

SessionSignaling::~SessionSignaling() {
	const auto session = _session;
	const auto id = _listenerId;
	if (id) {
		crl::on_main([=] {
			if (session) {
				session->removeListener(id);
			}
		});
	}
}

void SessionSignaling::subscribe() {
	Expects(_session != nullptr);

	const auto weak = weak_from_this();
	const auto participantsChanged = [=] {
		if (const auto strong = weak.lock()) {
			strong->notifyParticipants();
		}
	};
	_listenerId = _session->addListener({
		.joined = [=](const CallUser &) { participantsChanged(); },
		.left = [=](int64) { participantsChanged(); },
		.changed = [=](const CallUser &) { participantsChanged(); },
		.speaking = [=](int64 userId, bool speaking) {
			if (const auto strong = weak.lock()) {
				if (const auto callback = strong->events().speaking) {
					callback(userId, speaking);
				}
			}
		},
		.ended = [=](CallSession::EndReason, const QString &) {
			if (const auto strong = weak.lock()) {
				if (const auto callback = strong->events().ended) {
					callback();
				}
			}
		},
	});
}

Media::IceConfig SessionSignaling::iceConfig() const {
	return _ice;
}

int64_t SessionSignaling::selfId() const {
	return _selfId;
}

auto SessionSignaling::events() const -> Events {
	const auto lock = std::lock_guard(_mutex);
	return _events;
}

void SessionSignaling::setEvents(Events events) {
	{
		const auto lock = std::lock_guard(_mutex);
		_events = std::move(events);
	}
	const auto weak = weak_from_this();
	crl::on_main([=] {
		if (const auto strong = weak.lock()) {
			strong->notifyParticipants();
		}
	});
}

std::vector<Media::Participant> SessionSignaling::participants() const {
	auto result = std::vector<Media::Participant>();
	if (const auto session = _session.data()) {
		for (const auto &[id, user] : session->users()) {
			result.push_back({
				.id = id,
				.audio = user.audio,
				.video = user.video,
				.screen = (user.videoSource == u"screen"_q),
			});
		}
	}
	return result;
}

void SessionSignaling::notifyParticipants() {
	if (const auto callback = events().participants) {
		callback(participants());
	}
}

void SessionSignaling::sendOffer(
		int64_t dstUserId,
		std::string sdp,
		bool iceRestart,
		std::function<void(std::optional<std::string> answer)> done) {
	const auto session = _session;
	crl::on_main([=] {
		if (!session) {
			done(std::nullopt);
			return;
		}
		session->sendOffer(
			dstUserId,
			QByteArray::fromStdString(sdp),
			iceRestart,
			[=](QByteArray answer) { done(answer.toStdString()); },
			[=](QString) { done(std::nullopt); });
	});
}

void SessionSignaling::setAudioEnabled(bool enabled) {
	const auto session = _session;
	crl::on_main([=] {
		if (session) {
			session->setAudioEnabled(enabled);
		}
	});
}

void SessionSignaling::setVideoEnabled(bool enabled, bool screen) {
	const auto session = _session;
	crl::on_main([=] {
		if (session) {
			session->setVideoEnabled(
				enabled,
				screen ? u"screen"_q : u"video"_q);
		}
	});
}

void SessionSignaling::requestVideoQualities(
		std::map<int64_t, std::string> qualities) {
	const auto session = _session;
	crl::on_main([=] {
		if (session) {
			auto converted = std::map<int64, QString>();
			for (const auto &[id, quality] : qualities) {
				converted.emplace(id, QString::fromStdString(quality));
			}
			session->requestVideoQualities(converted);
		}
	});
}

void SessionSignaling::leave() {
	const auto session = _session;
	crl::on_main([=] {
		if (session) {
			session->leave();
		}
	});
}

} // namespace Pararam
