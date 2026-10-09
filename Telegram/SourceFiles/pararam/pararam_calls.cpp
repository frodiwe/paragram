/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_calls.h"

#include "pararam/pararam_backend.h"
#include "pararam/pararam_call_session.h"
#include "pararam/pararam_call_signaling.h"
#include "pararam/pararam_tl_reader.h"
#include "pararam/pararam_websocket.h"
#include "pararam/calls/pararam_call_instance.h"
#include "pararam/calls/pararam_media_signaling.h"
#include "base/openssl_help.h"
#include "base/random.h"
#include "base/unixtime.h"
#include "mtproto/mtproto_dh_utils.h"
#include "core/version.h"
#include "logs.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QTimeZone>
#include <QtCore/QTimer>

namespace Pararam {
namespace {

constexpr auto kMinLayer = 65;
constexpr auto kMaxLayer = 92;
constexpr auto kDhG = 3;
constexpr auto kDhVersion = 1;
constexpr auto kAuthKeySize = 256;
constexpr auto kRefreshFast = crl::time(3'000);
constexpr auto kRefreshSlow = crl::time(30'000);
constexpr auto kRefreshAfterEvent = crl::time(1'000);
constexpr auto kSocketHeartbeat = crl::time(15'000);
constexpr auto kSocketReconnectMin = crl::time(2'000);
constexpr auto kSocketReconnectMax = crl::time(60'000);
constexpr auto kIgnoreAfterFinish = crl::time(10'000);

// The 2048-bit safe prime Telegram uses, MTP::IsPrimeAndGood accepts it
// without the slow primality checks.
constexpr unsigned char kDhPrime[] = {
	0xC7, 0x1C, 0xAE, 0xB9, 0xC6, 0xB1, 0xC9, 0x04, 0x8E, 0x6C, 0x52, 0x2F, 0x70, 0xF1, 0x3F, 0x73,
	0x98, 0x0D, 0x40, 0x23, 0x8E, 0x3E, 0x21, 0xC1, 0x49, 0x34, 0xD0, 0x37, 0x56, 0x3D, 0x93, 0x0F,
	0x48, 0x19, 0x8A, 0x0A, 0xA7, 0xC1, 0x40, 0x58, 0x22, 0x94, 0x93, 0xD2, 0x25, 0x30, 0xF4, 0xDB,
	0xFA, 0x33, 0x6F, 0x6E, 0x0A, 0xC9, 0x25, 0x13, 0x95, 0x43, 0xAE, 0xD4, 0x4C, 0xCE, 0x7C, 0x37,
	0x20, 0xFD, 0x51, 0xF6, 0x94, 0x58, 0x70, 0x5A, 0xC6, 0x8C, 0xD4, 0xFE, 0x6B, 0x6B, 0x13, 0xAB,
	0xDC, 0x97, 0x46, 0x51, 0x29, 0x69, 0x32, 0x84, 0x54, 0xF1, 0x8F, 0xAF, 0x8C, 0x59, 0x5F, 0x64,
	0x24, 0x77, 0xFE, 0x96, 0xBB, 0x2A, 0x94, 0x1D, 0x5B, 0xCD, 0x1D, 0x4A, 0xC8, 0xCC, 0x49, 0x88,
	0x07, 0x08, 0xFA, 0x9B, 0x37, 0x8E, 0x3C, 0x4F, 0x3A, 0x90, 0x60, 0xBE, 0xE6, 0x7C, 0xF9, 0xA4,
	0xA4, 0xA6, 0x95, 0x81, 0x10, 0x51, 0x90, 0x7E, 0x16, 0x27, 0x53, 0xB5, 0x6B, 0x0F, 0x6B, 0x41,
	0x0D, 0xBA, 0x74, 0xD8, 0xA8, 0x4B, 0x2A, 0x14, 0xB3, 0x14, 0x4E, 0x0E, 0xF1, 0x28, 0x47, 0x54,
	0xFD, 0x17, 0xED, 0x95, 0x0D, 0x59, 0x65, 0xB4, 0xB9, 0xDD, 0x46, 0x58, 0x2D, 0xB1, 0x17, 0x8D,
	0x16, 0x9C, 0x6B, 0xC4, 0x65, 0xB0, 0xD6, 0xFF, 0x9C, 0xA3, 0x92, 0x8F, 0xEF, 0x5B, 0x9A, 0xE4,
	0xE4, 0x18, 0xFC, 0x15, 0xE8, 0x3E, 0xBE, 0xA0, 0xF8, 0x7F, 0xA9, 0xFF, 0x5E, 0xED, 0x70, 0x05,
	0x0D, 0xED, 0x28, 0x49, 0xF4, 0x7B, 0xF9, 0x59, 0xD9, 0x56, 0x85, 0x0C, 0xE9, 0x29, 0x85, 0x1F,
	0x0D, 0x81, 0x15, 0xF6, 0x35, 0xB1, 0x05, 0xEE, 0x2E, 0x4E, 0x15, 0xD0, 0x4B, 0x24, 0x54, 0xBF,
	0x6F, 0x4F, 0xAD, 0xF0, 0x34, 0xB1, 0x04, 0x03, 0x11, 0x9C, 0xD8, 0xE3, 0xB9, 0x2F, 0xCC, 0x5B };

[[nodiscard]] bytes::const_span DhPrime() {
	return bytes::make_span(kDhPrime);
}

[[nodiscard]] uint64 ComputeFingerprint(bytes::const_span key) {
	auto padded = bytes::vector(kAuthKeySize, gsl::byte());
	const auto size = std::min(int(key.size()), kAuthKeySize);
	bytes::copy(
		bytes::make_span(padded).subspan(kAuthKeySize - size),
		key.subspan(key.size() - size));
	const auto hash = openssl::Sha1(padded);
	auto result = uint64();
	for (auto i = 19; i >= 12; --i) {
		result = (result << 8) | gsl::to_integer<uint64>(hash[i]);
	}
	return result;
}

[[nodiscard]] QUrl SocketUrl() {
	const auto custom = qEnvironmentVariable("PARARAM_SOCK_URL");
	return QUrl((custom.isEmpty() ? u"wss://sock.pararam.io"_q : custom)
		+ u"/sock/websocket"_q);
}

} // namespace

struct CallManager::OneToOne {
	enum class State {
		Requesting, // Outgoing, waiting for the other user to join.
		Ringing, // Incoming, not answered yet.
		Accepting, // Incoming, answered, joining the room.
		Active,
	};

	uint64 id = 0;
	uint64 accessHash = 0;
	bool outgoing = false;
	bool video = false;
	int64 userId = 0;
	int64 chatId = 0;
	TimeId date = 0;
	TimeId startDate = 0;
	State state = State::Requesting;
	QString sessionId;

	// Diffie-Hellman with the UI, we play the other side.
	bytes::vector randomPower;
	bytes::vector ownModExp; // g_b for outgoing, g_a for incoming calls.
	bytes::vector gaHash; // Incoming.
	bytes::vector clientModExp; // g_a for outgoing calls.
	uint64 fingerprint = 0;

	std::unique_ptr<CallSession> session;
	std::shared_ptr<SessionSignaling> signaling;
	bool remoteJoined = false;
	bool approved = false;
};

struct CallManager::Group {
	int64 chatId = 0;
	uint64 id = 0;
	uint64 accessHash = 0;
	int32 version = 1;
	TimeId date = 0;
	std::vector<int64> listedUsers; // From GET /core/call.
	base::flat_map<int64, TimeId> joinDates;

	std::unique_ptr<CallSession> session;
	std::shared_ptr<SessionSignaling> signaling;
	uint32 selfSource = 0;
	bool joined = false;
	bool muted = true;
	bool videoStopped = true;
	TimeId selfJoinDate = 0;
	mtpRequestId joinRequestId = 0;
	bool listed = false;
};

CallManager::CallManager(not_null<Backend*> backend)
: _backend(backend)
, _socketReconnectTimer([=] { connectSocket(); })
, _socketHeartbeatTimer([=] {
	if (_socket && _socketConnected) {
		_socket->send("{\"hb\":0}");
	}
})
, _refreshTimer([=] { refreshCalls(); }) {
}

CallManager::~CallManager() {
	stop();
}

bool CallManager::handle(
		mtpRequestId requestId,
		mtpTypeId type,
		Reader &reader) {
	switch (type) {
	case mtpc_messages_getDhConfig: replyDhConfig(requestId, reader); break;
	case mtpc_phone_getCallConfig:
		_backend->reply(requestId, MTP_dataJSON(MTP_string(u"{}"_q)));
		break;
	case mtpc_phone_requestCall: requestCall(requestId, reader); break;
	case mtpc_phone_acceptCall: acceptCall(requestId, reader); break;
	case mtpc_phone_confirmCall: confirmCall(requestId, reader); break;
	case mtpc_phone_receivedCall: receivedCall(requestId, reader); break;
	case mtpc_phone_discardCall: discardCall(requestId, reader); break;
	case mtpc_phone_setCallRating:
		_backend->reply(requestId, _backend->wrapUpdates({}, {}, {}));
		break;
	case mtpc_phone_saveCallDebug:
	case mtpc_phone_sendSignalingData:
	case mtpc_phone_saveCallLog:
	case mtpc_phone_saveDefaultGroupCallJoinAs:
		_backend->replyTrue(requestId);
		break;
	case mtpc_phone_createGroupCall: createGroupCall(requestId, reader); break;
	case mtpc_phone_joinGroupCall: joinGroupCall(requestId, reader); break;
	case mtpc_phone_leaveGroupCall: leaveGroupCall(requestId, reader); break;
	case mtpc_phone_discardGroupCall:
		discardGroupCall(requestId, reader);
		break;
	case mtpc_phone_getGroupCall: getGroupCall(requestId, reader); break;
	case mtpc_phone_getGroupParticipants:
		getGroupParticipants(requestId, reader);
		break;
	case mtpc_phone_editGroupCallParticipant:
		editGroupCallParticipant(requestId, reader);
		break;
	case mtpc_phone_checkGroupCall: checkGroupCall(requestId, reader); break;
	case mtpc_phone_getGroupCallJoinAs:
		getGroupCallJoinAs(requestId, reader);
		break;
	case mtpc_phone_toggleGroupCallSettings:
	case mtpc_phone_editGroupCallTitle:
	case mtpc_phone_toggleGroupCallRecord:
	case mtpc_phone_toggleGroupCallStartSubscription:
		_backend->reply(requestId, _backend->wrapUpdates({}, {}, {}));
		break;
	case mtpc_phone_joinGroupCallPresentation:
		// Screen sharing in group calls is not supported yet,
		// use the camera button to share a window in private calls.
		_backend->replyError(requestId, 400, u"PARARAM_NOT_SUPPORTED"_q);
		break;
	case mtpc_phone_leaveGroupCallPresentation:
		_backend->reply(requestId, _backend->wrapUpdates({}, {}, {}));
		break;
	default: return false;
	}
	return true;
}

void CallManager::start() {
	if (_started) {
		return;
	}
	_started = true;
	connectSocket();
	refreshCalls();
}

void CallManager::stop() {
	_started = false;
	_socketReconnectTimer.cancel();
	_socketHeartbeatTimer.cancel();
	_refreshTimer.cancel();
	if (_socket) {
		_socket->closed = nullptr;
		_socket->close();
		_socket = nullptr;
	}
	_socketConnected = false;
	if (_call) {
		if (_call->session) {
			_call->session->leave();
		}
		_call = nullptr;
	}
	for (auto &[chatId, group] : base::take(_groups)) {
		if (group->session) {
			group->session->leave();
		}
	}
	_seenSessions.clear();
}

void CallManager::replyDhConfig(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto version = reader.int32Value();
	const auto length = reader.int32Value();
	auto random = QByteArray(std::clamp(length, 0, 1024), Qt::Uninitialized);
	base::RandomFill(random.data(), random.size());
	_backend->reply(requestId, MTP_messages_dhConfig(
		MTP_int(kDhG),
		MTP_bytes(DhPrime()),
		MTP_int(kDhVersion),
		MTP_bytes(random)));
}

MTPPhoneCallProtocol CallManager::protocol() const {
	return MTP_phoneCallProtocol(
		MTP_flags(MTPDphoneCallProtocol::Flag::f_udp_p2p
			| MTPDphoneCallProtocol::Flag::f_udp_reflector),
		MTP_int(kMinLayer),
		MTP_int(kMaxLayer),
		MTP_vector<MTPstring>(QVector<MTPstring>{
			MTP_string(QString::fromLatin1(Media::kCallVersion)),
		}));
}

bool CallManager::checkCall(const MTPInputPhoneCall &peer) const {
	const auto &data = peer.c_inputPhoneCall();
	return _call
		&& (uint64(data.vid().v) == _call->id)
		&& (uint64(data.vaccess_hash().v) == _call->accessHash);
}

int32 CallManager::duration() const {
	return (_call && _call->startDate)
		? std::max(int32(base::unixtime::now() - _call->startDate), 0)
		: 0;
}

MTPPhoneCall CallManager::waitingToMTP(bool received) const {
	using Flag = MTPDphoneCallWaiting::Flag;
	const auto self = _backend->_selfId;
	return MTP_phoneCallWaiting(
		MTP_flags((_call->video ? Flag::f_video : Flag())
			| (received ? Flag::f_receive_date : Flag())),
		MTP_long(_call->id),
		MTP_long(_call->accessHash),
		MTP_int(_call->date),
		MTP_long(_call->outgoing ? self : _call->userId),
		MTP_long(_call->outgoing ? _call->userId : self),
		protocol(),
		MTP_int(received ? base::unixtime::now() : 0));
}

MTPPhoneCall CallManager::activeToMTP() const {
	using Flag = MTPDphoneCall::Flag;
	const auto self = _backend->_selfId;
	const auto modExp = _call->outgoing
		? _call->clientModExp
		: _call->ownModExp;
	return MTP_phoneCall(
		MTP_flags(_call->video ? Flag::f_video : Flag()),
		MTP_long(_call->id),
		MTP_long(_call->accessHash),
		MTP_int(_call->date),
		MTP_long(_call->outgoing ? self : _call->userId),
		MTP_long(_call->outgoing ? _call->userId : self),
		MTP_bytes(modExp),
		MTP_long(_call->fingerprint),
		protocol(),
		MTP_vector<MTPPhoneConnection>(),
		MTP_int(_call->startDate ? _call->startDate : base::unixtime::now()),
		MTPDataJSON());
}

MTPPhoneCall CallManager::discardedToMTP(
		const MTPPhoneCallDiscardReason &reason) const {
	using Flag = MTPDphoneCallDiscarded::Flag;
	const auto seconds = duration();
	return MTP_phoneCallDiscarded(
		MTP_flags(Flag::f_reason
			| (seconds ? Flag::f_duration : Flag())
			| (_call->video ? Flag::f_video : Flag())),
		MTP_long(_call->id),
		reason,
		MTP_int(seconds));
}

void CallManager::pushCall(const MTPPhoneCall &call) {
	auto users = std::vector<int64>();
	if (_call) {
		users.push_back(_call->userId);
	}
	_backend->pushUpdates(_backend->wrapUpdates(
		{ MTP_updatePhoneCall(call) },
		users,
		{}));
}

void CallManager::requestCall(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto userId = _backend->userIdFromInputUser(
		reader.read<MTPInputUser>());
	[[maybe_unused]] const auto randomId = reader.int32Value();
	[[maybe_unused]] const auto gaHash = reader.bytes();
	[[maybe_unused]] const auto clientProtocol
		= reader.read<MTPPhoneCallProtocol>();
	if (!reader.ok() || !userId || userId == _backend->_selfId) {
		_backend->replyError(requestId, 400, u"USER_ID_INVALID"_q);
		return;
	} else if (_call) {
		_backend->replyError(requestId, 400, u"CALL_ALREADY_ACTIVE"_q);
		return;
	}
	const auto video = (flags & (1U << 0)) != 0;
	const auto weak = QPointer<CallManager>(this);
	_backend->withChatForPeer(MTP_inputPeerUser(
		MTP_long(userId),
		MTP_long(0)
	), [=](Backend::Chat *chat) {
		if (!weak) {
			return;
		} else if (!chat) {
			_backend->replyError(requestId, 400, u"USER_ID_INVALID"_q);
			return;
		} else if (_call) {
			_backend->replyError(requestId, 400, u"CALL_ALREADY_ACTIVE"_q);
			return;
		}
		auto random = bytes::vector(MTP::ModExpFirst::kRandomPowerSize);
		base::RandomFill(bytes::make_span(random));
		auto first = MTP::CreateModExp(kDhG, DhPrime(), random);
		if (first.modexp.empty()) {
			_backend->replyError(requestId, 500, u"PARARAM_DH_FAILED"_q);
			return;
		}
		_call = std::make_unique<OneToOne>();
		_call->id = base::RandomValue<uint64>() & 0x7FFFFFFFFFFFFFFFULL;
		_call->accessHash = base::RandomValue<uint64>();
		_call->outgoing = true;
		_call->video = video;
		_call->userId = userId;
		_call->chatId = chat->info.id;
		_call->date = base::unixtime::now();
		_call->state = OneToOne::State::Requesting;
		_call->randomPower = std::move(first.randomPower);
		_call->ownModExp = std::move(first.modexp);

		_backend->reply(requestId, MTP_phone_phoneCall(
			waitingToMTP(false),
			_backend->usersToMTP({ _backend->_selfId, userId })));
		startSession(true);
	}, _backend->failer(requestId), true);
}

void CallManager::receivedCall(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto peer = reader.read<MTPInputPhoneCall>();
	_backend->replyTrue(requestId);
}

void CallManager::acceptCall(mtpRequestId requestId, Reader &reader) {
	const auto peer = reader.read<MTPInputPhoneCall>();
	const auto gb = reader.bytes();
	[[maybe_unused]] const auto clientProtocol
		= reader.read<MTPPhoneCallProtocol>();
	if (!reader.ok() || !checkCall(peer)) {
		_backend->replyError(requestId, 400, u"CALL_PEER_INVALID"_q);
		return;
	} else if (_call->outgoing
		|| _call->state != OneToOne::State::Ringing) {
		_backend->replyError(requestId, 400, u"CALL_ALREADY_ACCEPTED"_q);
		return;
	}
	const auto key = MTP::CreateAuthKey(
		bytes::make_span(gb),
		_call->randomPower,
		DhPrime());
	if (key.empty()) {
		_backend->replyError(requestId, 400, u"CALL_PROTOCOL_FLAGS_INVALID"_q);
		return;
	}
	_call->fingerprint = ComputeFingerprint(key);
	_call->state = OneToOne::State::Accepting;
	_backend->reply(requestId, MTP_phone_phoneCall(
		waitingToMTP(true),
		_backend->usersToMTP({ _backend->_selfId, _call->userId })));
	startSession(false);
}

void CallManager::confirmCall(mtpRequestId requestId, Reader &reader) {
	const auto peer = reader.read<MTPInputPhoneCall>();
	const auto ga = reader.bytes();
	const auto fingerprint = reader.int64Value();
	[[maybe_unused]] const auto clientProtocol
		= reader.read<MTPPhoneCallProtocol>();
	if (!reader.ok() || !checkCall(peer) || !_call->outgoing) {
		_backend->replyError(requestId, 400, u"CALL_PEER_INVALID"_q);
		return;
	} else if (!_call->session || !_call->approved || !_call->remoteJoined) {
		_backend->replyError(requestId, 400, u"CALL_ALREADY_DECLINED"_q);
		return;
	}
	_call->clientModExp = bytes::make_vector(bytes::make_span(ga));
	_call->fingerprint = fingerprint;
	_call->state = OneToOne::State::Active;
	_call->startDate = base::unixtime::now();
	startMedia();
	_backend->reply(requestId, MTP_phone_phoneCall(
		activeToMTP(),
		_backend->usersToMTP({ _backend->_selfId, _call->userId })));
}

void CallManager::discardCall(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPhoneCall>();
	[[maybe_unused]] const auto clientDuration = reader.int32Value();
	const auto reason = reader.read<MTPPhoneCallDiscardReason>();
	[[maybe_unused]] const auto connectionId = reader.int64Value();
	if (!reader.ok() || !checkCall(peer)) {
		const auto &data = peer.c_inputPhoneCall();
		using Flag = MTPDphoneCallDiscarded::Flag;
		_backend->reply(requestId, _backend->wrapUpdates({
			MTP_updatePhoneCall(MTP_phoneCallDiscarded(
				MTP_flags(Flag::f_reason),
				data.vid(),
				MTP_phoneCallDiscardReasonHangup(),
				MTPint()))
		}, {}, {}));
		return;
	}
	if (_call->session) {
		_call->session->leave();
	} else if (!_call->outgoing
		&& _call->state == OneToOne::State::Ringing) {
		CallSession::Reject(&_backend->_api, _call->chatId, _backend->_selfId);
	}
	if (!_call->sessionId.isEmpty()) {
		_seenSessions.emplace(_call->sessionId);
	}
	_finishedAt[_call->chatId] = crl::now();
	const auto users = std::vector<int64>{ _call->userId };
	const auto discarded = discardedToMTP(reason);
	_call = nullptr;
	_backend->reply(requestId, _backend->wrapUpdates(
		{ MTP_updatePhoneCall(discarded) },
		users,
		{}));
}

void CallManager::startSession(bool outgoing) {
	Expects(_call != nullptr);

	_call->session = std::make_unique<CallSession>(
		&_backend->_api,
		CallSession::Options{
			.chatId = _call->chatId,
			.selfId = _backend->_selfId,
			.selfName = _backend->_self.name,
			.silent = false,
			.stayInCall = false,
		});
	const auto session = _call->session.get();
	const auto weak = QPointer<CallManager>(this);
	const auto alive = [=] {
		return weak && _call && _call->session.get() == session;
	};
	session->addListener({
		.approved = [=] {
			if (alive()) {
				sessionApproved();
			}
		},
		.joined = [=](const CallUser &user) {
			if (alive()) {
				sessionJoined(user.id);
			}
		},
		.left = [=](int64 userId) {
			if (alive()) {
				sessionLeft(userId);
			}
		},
		.ended = [=](CallSession::EndReason, const QString &) {
			if (alive()) {
				sessionEnded();
			}
		},
	});
	session->start();
}

void CallManager::sessionApproved() {
	_call->approved = true;
	if (_call->outgoing) {
		// The UI starts ringing on receive_date.
		pushCall(waitingToMTP(true));
		if (_call->session->users().contains(_call->userId)) {
			sessionJoined(_call->userId);
		}
	} else if (_call->state == OneToOne::State::Accepting) {
		_call->state = OneToOne::State::Active;
		_call->startDate = base::unixtime::now();
		startMedia();
		pushCall(activeToMTP());
	}
}

void CallManager::sessionJoined(int64 userId) {
	if (userId != _call->userId) {
		return;
	}
	_call->remoteJoined = true;
	if (_call->outgoing
		&& _call->approved
		&& _call->state == OneToOne::State::Requesting) {
		pushCall(MTP_phoneCallAccepted(
			MTP_flags(_call->video
				? MTPDphoneCallAccepted::Flag::f_video
				: MTPDphoneCallAccepted::Flag()),
			MTP_long(_call->id),
			MTP_long(_call->accessHash),
			MTP_int(_call->date),
			MTP_long(_backend->_selfId),
			MTP_long(_call->userId),
			MTP_bytes(_call->ownModExp),
			protocol()));
	}
}

void CallManager::sessionLeft(int64 userId) {
	if (userId != _call->userId || !_call->remoteJoined) {
		return;
	}
	finishCall(MTP_phoneCallDiscardReasonHangup());
}

void CallManager::sessionEnded() {
	const auto missed = _call->outgoing && !_call->remoteJoined;
	finishCall(missed
		? MTP_phoneCallDiscardReasonMissed()
		: MTP_phoneCallDiscardReasonHangup());
}

void CallManager::startMedia() {
	Expects(_call && _call->session);

	_call->signaling = std::make_shared<SessionSignaling>(
		_call->session.get());
	_call->signaling->subscribe();
	Media::SetPendingSignaling(_call->signaling);
}

void CallManager::finishCall(const MTPPhoneCallDiscardReason &reason) {
	if (!_call) {
		return;
	}
	if (_call->session) {
		_call->session->leave();
	}
	if (!_call->sessionId.isEmpty()) {
		_seenSessions.emplace(_call->sessionId);
	}
	_finishedAt[_call->chatId] = crl::now();
	const auto discarded = discardedToMTP(reason);
	const auto userId = _call->userId;
	// The session may be notifying us right now, destroy it later.
	if (const auto session = _call->session.release()) {
		session->deleteLater();
	}
	_call = nullptr;
	_backend->pushUpdates(_backend->wrapUpdates(
		{ MTP_updatePhoneCall(discarded) },
		{ userId },
		{}));
	scheduleRefresh(kRefreshAfterEvent);
}

// Group calls.

MTPInputGroupCall CallManager::groupCallInput(int64 chatId) const {
	const auto i = _groups.find(chatId);
	return (i != end(_groups))
		? groupInputToMTP(*i->second)
		: MTPInputGroupCall();
}

bool CallManager::groupCallActive(int64 chatId) const {
	return _groups.contains(chatId);
}

bool CallManager::groupCallNotEmpty(int64 chatId) const {
	const auto i = _groups.find(chatId);
	return (i != end(_groups)) && !groupUsers(*i->second).empty();
}

auto CallManager::groupByInput(const MTPInputGroupCall &call) const
-> Group* {
	return call.match([&](const MTPDinputGroupCall &data) -> Group* {
		for (const auto &[chatId, group] : _groups) {
			if (group->id == uint64(data.vid().v)) {
				return group.get();
			}
		}
		return nullptr;
	}, [](const auto &) -> Group* {
		return nullptr;
	});
}

auto CallManager::ensureGroup(int64 chatId) -> Group& {
	auto &result = _groups[chatId];
	if (!result) {
		result = std::make_unique<Group>();
		result->chatId = chatId;
		// Unique while the app runs, the call UI keys its data by the id.
		result->id = (uint64(chatId) << 16) | (++_groupCallIdCounter & 0xFFFF);
		result->accessHash = base::RandomValue<uint64>();
		result->date = base::unixtime::now();
	}
	return *result;
}

std::vector<int64> CallManager::groupUsers(const Group &group) const {
	auto result = std::vector<int64>();
	if (group.session && group.joined) {
		for (const auto &[id, user] : group.session->users()) {
			if (!user.external) {
				result.push_back(id);
			}
		}
		result.push_back(_backend->_selfId);
	} else {
		for (const auto id : group.listedUsers) {
			if (id != _backend->_selfId) {
				result.push_back(id);
			}
		}
	}
	return result;
}

MTPInputGroupCall CallManager::groupInputToMTP(const Group &group) const {
	return MTP_inputGroupCall(
		MTP_long(group.id),
		MTP_long(group.accessHash));
}

MTPGroupCall CallManager::groupToMTP(const Group &group) const {
	using Flag = MTPDgroupCall::Flag;
	return MTP_groupCall(
		MTP_flags(Flag::f_can_start_video),
		MTP_long(group.id),
		MTP_long(group.accessHash),
		MTP_int(int(groupUsers(group).size())),
		MTPstring(), // title
		MTPint(), // stream_dc_id
		MTPint(), // record_start_date
		MTPint(), // schedule_date
		MTPint(), // unmuted_video_count
		MTP_int(1000), // unmuted_video_limit
		MTP_int(group.version),
		MTPstring(), // invite_link
		MTPlong(), // send_paid_messages_stars
		MTPPeer()); // default_send_as
}

MTPGroupCallParticipant CallManager::participantToMTP(
		const Group &group,
		int64 userId,
		bool left) const {
	using Flag = MTPDgroupCallParticipant::Flag;
	const auto self = (userId == _backend->_selfId);
	const auto date = [&] {
		if (self && group.selfJoinDate) {
			return group.selfJoinDate;
		}
		const auto i = group.joinDates.find(userId);
		return (i != end(group.joinDates)) ? i->second : group.date;
	}();
	auto flags = Flag::f_can_self_unmute | Flag::f_versioned;
	auto video = MTPGroupCallParticipantVideo();
	auto presentation = MTPGroupCallParticipantVideo();
	auto source = Media::AudioSourceFor(userId);
	if (left) {
		flags |= Flag::f_left;
	}
	if (self) {
		flags |= Flag::f_self;
		source = group.selfSource;
		if (group.muted) {
			flags |= Flag::f_muted;
		}
	} else if (group.session && group.joined) {
		const auto &users = group.session->users();
		const auto i = users.find(userId);
		if (i != end(users)) {
			const auto &user = i->second;
			if (!user.audio) {
				flags |= Flag::f_muted;
			}
			if (user.video) {
				const auto description = MTP_groupCallParticipantVideo(
					MTP_flags(0),
					MTP_string(Media::EndpointFor(userId)),
					MTP_vector<MTPGroupCallParticipantVideoSourceGroup>(
						1,
						MTP_groupCallParticipantVideoSourceGroup(
							MTP_string("SIM"),
							MTP_vector<MTPint>(1, MTP_int(
								Media::VideoSourceFor(userId))))),
					MTPint());
				if (user.videoSource == u"screen"_q) {
					flags |= Flag::f_presentation;
					presentation = description;
				} else {
					flags |= Flag::f_video;
					video = description;
				}
			}
		}
	}
	return MTP_groupCallParticipant(
		MTP_flags(flags),
		MTP_peerUser(MTP_long(userId)),
		MTP_int(date),
		MTPint(), // active_date
		MTP_int(int32(source)),
		MTPint(), // volume
		MTPstring(), // about
		MTPlong(), // raise_hand_rating
		video,
		presentation,
		MTPlong()); // paid_stars_total
}

QVector<MTPGroupCallParticipant> CallManager::participantsToMTP(
		const Group &group) const {
	auto result = QVector<MTPGroupCallParticipant>();
	for (const auto userId : groupUsers(group)) {
		result.push_back(participantToMTP(group, userId));
	}
	return result;
}

MTPUpdates CallManager::groupUpdates(
		const Group &group,
		QVector<MTPUpdate> updates) const {
	return _backend->wrapUpdates(
		std::move(updates),
		groupUsers(group),
		{ group.chatId });
}

void CallManager::pushParticipants(
		Group &group,
		QVector<MTPGroupCallParticipant> participants) {
	if (participants.isEmpty()) {
		return;
	}
	++group.version;
	_backend->pushUpdates(groupUpdates(group, {
		MTP_updateGroupCallParticipants(
			groupInputToMTP(group),
			MTP_vector<MTPGroupCallParticipant>(std::move(participants)),
			MTP_int(group.version)),
	}));
}

void CallManager::createGroupCall(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPeer>();
	[[maybe_unused]] const auto randomId = reader.int32Value();
	const auto chat = _backend->chatByInputPeer(peer);
	if (!chat || chat->info.pm) {
		_backend->replyError(requestId, 400, u"PEER_ID_INVALID"_q);
		return;
	}
	auto &group = ensureGroup(chat->info.id);
	_backend->reply(requestId, groupUpdates(group, {
		MTP_updateGroupCall(
			MTP_flags(MTPDupdateGroupCall::Flag::f_peer),
			_backend->peerToMTP(*chat),
			groupToMTP(group)),
	}));
}

void CallManager::joinGroupCall(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto call = reader.read<MTPInputGroupCall>();
	[[maybe_unused]] const auto joinAs = reader.read<MTPInputPeer>();
	if (flags & (1U << 1)) {
		[[maybe_unused]] const auto inviteHash = reader.string();
	}
	if (flags & (1U << 3)) {
		[[maybe_unused]] const auto key = reader.read<MTPint256>();
		[[maybe_unused]] const auto block = reader.bytes();
	}
	const auto params = reader.read<MTPDataJSON>();
	const auto group = groupByInput(call);
	if (!reader.ok() || !group) {
		_backend->replyError(requestId, 400, u"GROUPCALL_INVALID"_q);
		return;
	} else if (group->joinRequestId || group->joined) {
		_backend->replyError(requestId, 400, u"GROUPCALL_ALREADY_JOINED"_q);
		return;
	}
	const auto json = QJsonDocument::fromJson(
		params.c_dataJSON().vdata().v).object();
	group->selfSource = uint32(json.value(u"ssrc"_q).toDouble());
	group->muted = (flags & (1U << 0)) != 0;
	group->videoStopped = (flags & (1U << 2)) != 0;
	group->joinRequestId = requestId;
	startGroupSession(*group);
}

void CallManager::startGroupSession(Group &group) {
	group.session = std::make_unique<CallSession>(
		&_backend->_api,
		CallSession::Options{
			.chatId = group.chatId,
			.selfId = _backend->_selfId,
			.selfName = _backend->_self.name,
			.silent = true,
			.stayInCall = true,
		});
	const auto chatId = group.chatId;
	const auto session = group.session.get();
	const auto weak = QPointer<CallManager>(this);
	const auto alive = [=] {
		if (!weak) {
			return false;
		}
		const auto i = _groups.find(chatId);
		return (i != end(_groups)) && (i->second->session.get() == session);
	};
	session->addListener({
		.approved = [=] {
			if (alive()) {
				groupApproved(chatId);
			}
		},
		.joined = [=](const CallUser &user) {
			if (alive()) {
				groupChanged(chatId, user.id, false);
			}
		},
		.left = [=](int64 userId) {
			if (alive()) {
				groupChanged(chatId, userId, true);
			}
		},
		.changed = [=](const CallUser &user) {
			if (alive()) {
				groupChanged(chatId, user.id, false);
			}
		},
		.ended = [=](CallSession::EndReason, const QString &) {
			if (alive()) {
				groupEnded(chatId);
			}
		},
	});
	session->start();
}

void CallManager::groupApproved(int64 chatId) {
	auto &group = *_groups[chatId];
	const auto now = base::unixtime::now();
	group.joined = true;
	group.selfJoinDate = now;
	for (const auto &[id, user] : group.session->users()) {
		group.joinDates.emplace(id, now);
	}
	group.signaling = std::make_shared<SessionSignaling>(group.session.get());
	group.signaling->subscribe();
	Media::SetPendingSignaling(group.signaling);

	auto response = QJsonObject();
	auto video = QJsonObject();
	video.insert(
		u"endpoint"_q,
		QString::fromStdString(Media::EndpointFor(_backend->_selfId)));
	response.insert(u"video"_q, video);
	response.insert(u"pararam"_q, true);

	++group.version;
	const auto chat = _backend->chatById(chatId);
	auto updates = QVector<MTPUpdate>{
		MTP_updateGroupCallConnection(
			MTP_flags(0),
			MTP_dataJSON(MTP_bytes(
				QJsonDocument(response).toJson(QJsonDocument::Compact)))),
		MTP_updateGroupCallParticipants(
			groupInputToMTP(group),
			MTP_vector<MTPGroupCallParticipant>(participantsToMTP(group)),
			MTP_int(group.version)),
	};
	if (chat) {
		updates.push_back(MTP_updateGroupCall(
			MTP_flags(MTPDupdateGroupCall::Flag::f_peer),
			_backend->peerToMTP(*chat),
			groupToMTP(group)));
	}
	_backend->reply(
		std::exchange(group.joinRequestId, 0),
		groupUpdates(group, std::move(updates)));
}

void CallManager::groupChanged(int64 chatId, int64 userId, bool left) {
	auto &group = *_groups[chatId];
	if (!group.joined) {
		return;
	}
	if (left) {
		group.joinDates.remove(userId);
	} else {
		group.joinDates.emplace(userId, base::unixtime::now());
	}
	const auto users = std::vector<int64>{ userId };
	_backend->ensureUsers(users, [=] {
		const auto i = _groups.find(chatId);
		if (i == end(_groups) || !i->second->joined) {
			return;
		}
		pushParticipants(*i->second, {
			participantToMTP(*i->second, userId, left),
		});
	});
}

void CallManager::groupEnded(int64 chatId) {
	auto &group = *_groups[chatId];
	if (const auto requestId = std::exchange(group.joinRequestId, 0)) {
		_backend->replyError(requestId, 400, u"GROUPCALL_JOIN_FAILED"_q);
		finishGroupSession(group);
		return;
	}
	if (group.joined) {
		pushParticipants(group, {
			participantToMTP(group, _backend->_selfId, true),
		});
	}
	finishGroupSession(group);
	scheduleRefresh(kRefreshAfterEvent);
}

void CallManager::finishGroupSession(Group &group) {
	group.joined = false;
	group.signaling = nullptr;
	if (const auto session = group.session.release()) {
		session->leave();
		session->deleteLater();
	}
}

void CallManager::leaveGroupCall(mtpRequestId requestId, Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	[[maybe_unused]] const auto source = reader.int32Value();
	const auto group = groupByInput(call);
	if (!group) {
		_backend->reply(requestId, _backend->wrapUpdates({}, {}, {}));
		return;
	}
	const auto wasJoined = group->joined;
	auto self = participantToMTP(*group, _backend->_selfId, true);
	finishGroupSession(*group);
	if (wasJoined) {
		++group->version;
	}
	_backend->reply(requestId, groupUpdates(*group, wasJoined
		? QVector<MTPUpdate>{ MTP_updateGroupCallParticipants(
			groupInputToMTP(*group),
			MTP_vector<MTPGroupCallParticipant>(1, self),
			MTP_int(group->version)) }
		: QVector<MTPUpdate>()));
	scheduleRefresh(kRefreshAfterEvent);
}

void CallManager::discardGroupCall(mtpRequestId requestId, Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	const auto group = groupByInput(call);
	if (!group) {
		_backend->reply(requestId, _backend->wrapUpdates({}, {}, {}));
		return;
	}
	// pararam.io calls end when everyone left, we only leave.
	finishGroupSession(*group);
	_backend->reply(requestId, groupUpdates(*group, {}));
	scheduleRefresh(kRefreshAfterEvent);
}

void CallManager::getGroupCall(mtpRequestId requestId, Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	[[maybe_unused]] const auto limit = reader.int32Value();
	const auto group = groupByInput(call);
	if (!group) {
		_backend->replyError(requestId, 400, u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto chatId = group->chatId;
	const auto weak = QPointer<CallManager>(this);
	_backend->ensureUsers(groupUsers(*group), [=] {
		const auto i = weak ? _groups.find(chatId) : end(_groups);
		if (!weak || i == end(_groups)) {
			_backend->replyError(requestId, 400, u"GROUPCALL_INVALID"_q);
			return;
		}
		const auto &group = *i->second;
		_backend->reply(requestId, MTP_phone_groupCall(
			groupToMTP(group),
			MTP_vector<MTPGroupCallParticipant>(participantsToMTP(group)),
			MTP_string(),
			_backend->chatsToMTP({ group.chatId }),
			_backend->usersToMTP(groupUsers(group))));
	});
}

void CallManager::getGroupParticipants(
		mtpRequestId requestId,
		Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	const auto group = groupByInput(call);
	if (!group) {
		_backend->replyError(requestId, 400, u"GROUPCALL_INVALID"_q);
		return;
	}
	const auto chatId = group->chatId;
	const auto weak = QPointer<CallManager>(this);
	_backend->ensureUsers(groupUsers(*group), [=] {
		const auto i = weak ? _groups.find(chatId) : end(_groups);
		if (!weak || i == end(_groups)) {
			_backend->replyError(requestId, 400, u"GROUPCALL_INVALID"_q);
			return;
		}
		const auto &group = *i->second;
		const auto participants = participantsToMTP(group);
		_backend->reply(requestId, MTP_phone_groupParticipants(
			MTP_int(participants.size()),
			MTP_vector<MTPGroupCallParticipant>(participants),
			MTP_string(),
			_backend->chatsToMTP({ group.chatId }),
			_backend->usersToMTP(groupUsers(group)),
			MTP_int(group.version)));
	});
}

void CallManager::editGroupCallParticipant(
		mtpRequestId requestId,
		Reader &reader) {
	const auto flags = reader.flags();
	const auto call = reader.read<MTPInputGroupCall>();
	const auto participant = reader.read<MTPInputPeer>();
	auto muted = std::optional<bool>();
	auto videoStopped = std::optional<bool>();
	if (flags & (1U << 0)) {
		muted = mtpIsTrue(reader.read<MTPBool>());
	}
	if (flags & (1U << 1)) {
		[[maybe_unused]] const auto volume = reader.int32Value();
	}
	if (flags & (1U << 2)) {
		[[maybe_unused]] const auto raiseHand = reader.read<MTPBool>();
	}
	if (flags & (1U << 3)) {
		videoStopped = mtpIsTrue(reader.read<MTPBool>());
	}
	const auto group = groupByInput(call);
	const auto self = (_backend->userIdFromInputPeer(participant)
		== _backend->_selfId);
	if (!reader.ok() || !group || !self || !group->joined) {
		_backend->reply(requestId, _backend->wrapUpdates({}, {}, {}));
		return;
	}
	if (muted) {
		group->muted = *muted;
	}
	if (videoStopped) {
		group->videoStopped = *videoStopped;
	}
	++group->version;
	_backend->reply(requestId, groupUpdates(*group, {
		MTP_updateGroupCallParticipants(
			groupInputToMTP(*group),
			MTP_vector<MTPGroupCallParticipant>(
				1,
				participantToMTP(*group, _backend->_selfId)),
			MTP_int(group->version)),
	}));
}

void CallManager::checkGroupCall(mtpRequestId requestId, Reader &reader) {
	const auto call = reader.read<MTPInputGroupCall>();
	const auto sources = reader.read<MTPVector<MTPint>>();
	const auto group = groupByInput(call);
	_backend->reply(requestId, (group && group->joined)
		? sources
		: MTPVector<MTPint>(MTP_vector<MTPint>()));
}

void CallManager::getGroupCallJoinAs(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto peer = reader.read<MTPInputPeer>();
	_backend->reply(requestId, MTP_phone_joinAsPeers(
		MTP_vector<MTPPeer>(1, MTP_peerUser(MTP_long(_backend->_selfId))),
		MTP_vector<MTPChat>(),
		_backend->usersToMTP({ _backend->_selfId })));
}

void CallManager::applyGroupCalls(const std::vector<ActiveCall> &calls) {
	auto seen = base::flat_set<int64>();
	for (const auto &call : calls) {
		const auto chat = _backend->chatById(call.chatId);
		if (!chat || chat->info.pm) {
			continue;
		}
		seen.emplace(call.chatId);
		const auto known = _groups.contains(call.chatId);
		auto &group = ensureGroup(call.chatId);
		const auto was = group.listedUsers;
		group.listed = true;
		group.listedUsers = call.users;
		for (const auto id : call.users) {
			group.joinDates.emplace(id, base::unixtime::now());
		}
		if (!known) {
			// Make the UI reload channelFull and see the call.
			_backend->pushUpdates(_backend->wrapUpdates({
				MTP_updateChannel(MTP_long(call.chatId)),
			}, {}, { call.chatId }));
		} else if (!group.joined && was != call.users) {
			auto changes = QVector<MTPGroupCallParticipant>();
			for (const auto id : call.users) {
				if (!ranges::contains(was, id) && id != _backend->_selfId) {
					changes.push_back(participantToMTP(group, id));
				}
			}
			for (const auto id : was) {
				if (!ranges::contains(call.users, id)
					&& id != _backend->_selfId) {
					changes.push_back(participantToMTP(group, id, true));
				}
			}
			const auto chatId = call.chatId;
			const auto weak = QPointer<CallManager>(this);
			_backend->ensureUsers(call.users, [=] {
				const auto i = weak ? _groups.find(chatId) : end(_groups);
				if (weak && i != end(_groups)) {
					pushParticipants(*i->second, changes);
				}
			});
		}
	}
	auto removed = std::vector<int64>();
	for (const auto &[chatId, group] : _groups) {
		if (!seen.contains(chatId)
			&& !group->joined
			&& !group->joinRequestId
			&& (group->listed
				|| base::unixtime::now() - group->date > 60)) {
			removed.push_back(chatId);
		}
	}
	for (const auto chatId : removed) {
		removeGroup(chatId);
	}
}

void CallManager::removeGroup(int64 chatId) {
	const auto i = _groups.find(chatId);
	if (i == end(_groups)) {
		return;
	}
	auto group = std::move(i->second);
	_groups.erase(i);
	finishGroupSession(*group);
	const auto chat = _backend->chatById(chatId);
	if (!chat) {
		return;
	}
	_backend->pushUpdates(_backend->wrapUpdates({
		MTP_updateGroupCall(
			MTP_flags(MTPDupdateGroupCall::Flag::f_peer),
			_backend->peerToMTP(*chat),
			MTP_groupCallDiscarded(
				MTP_long(group->id),
				MTP_long(group->accessHash),
				MTP_int(std::max(
					int(base::unixtime::now() - group->date),
					0)))),
	}, {}, { chatId }));
}

// Watching for incoming calls.

void CallManager::connectSocket() {
	if (!_started || _socket) {
		return;
	}
	const auto url = SocketUrl();
	auto cookies = _backend->_api.cookieHeader(
		QUrl(u"https://"_q + url.host() + '/'));
	const auto apiCookies = _backend->_api.cookieHeader(
		QUrl(_backend->_api.apiUrl()));
	if (cookies.isEmpty()) {
		cookies = apiCookies;
	}
	_socket = std::make_unique<WebSocket>();
	const auto socket = _socket.get();
	socket->opened = [=] {
		_socketConnected = true;
		_socketFailures = 0;
		auto data = QJsonObject();
		data.insert(
			u"timezone_offs"_q,
			QDateTime::currentDateTime().offsetFromUtc() / 60);
		auto message = QJsonObject();
		message.insert(u"msg"_q, u"initial_info"_q);
		message.insert(u"data"_q, data);
		socket->send(QJsonDocument(message).toJson(QJsonDocument::Compact));
		_socketHeartbeatTimer.callEach(kSocketHeartbeat);
		scheduleRefresh(0);
	};
	socket->received = [=](const QByteArray &text) {
		const auto message = QJsonDocument::fromJson(text).object();
		const auto type = message.value(u"msg"_q).toString();
		if (type == u"call"_q) {
			scheduleRefresh(kRefreshAfterEvent);
		} else if (type == u"unauthorized"_q) {
			LOG(("Pararam: Main socket unauthorized."));
			socket->close();
		}
	};
	socket->closed = [=](int code, const QString &reason) {
		LOG(("Pararam: Main socket closed, code %1, reason: %2"
			).arg(code
			).arg(reason));
		// Do not destroy the socket from inside its own callback.
		QTimer::singleShot(0, this, [=] { socketClosed(); });
	};
	auto headers = std::vector<WebSocket::Header>{
		{ "Origin", "https://app.pararam.io" },
		{ "User-Agent", "Pararam Desktop/" + QByteArray(AppVersionStr) },
	};
	if (!cookies.isEmpty()) {
		headers.push_back({ "Cookie", cookies });
	}
	socket->open(url, std::move(headers));
}

void CallManager::socketClosed() {
	_socket = nullptr;
	_socketConnected = false;
	_socketHeartbeatTimer.cancel();
	if (!_started) {
		return;
	}
	++_socketFailures;
	const auto delay = std::min(
		kSocketReconnectMin << std::min(_socketFailures - 1, 5),
		kSocketReconnectMax);
	_socketReconnectTimer.callOnce(delay);
	scheduleRefresh(kRefreshFast);
}

void CallManager::scheduleRefresh(crl::time delay) {
	if (!_started) {
		return;
	}
	if (!_refreshTimer.isActive() || _refreshTimer.remainingTime() > delay) {
		_refreshTimer.callOnce(delay);
	}
}

void CallManager::refreshCalls() {
	if (!_started || !_backend->authorized()) {
		return;
	} else if (_refreshing) {
		_refreshAgain = true;
		return;
	}
	_refreshing = true;
	const auto weak = QPointer<CallManager>(this);
	const auto next = [=] {
		const auto fast = !_socketConnected
			|| (_call && _call->state != OneToOne::State::Active);
		scheduleRefresh(fast ? kRefreshFast : kRefreshSlow);
	};
	_backend->_api.get(u"/core/call"_q, [=](const QJsonValue &result) {
		if (!weak) {
			return;
		}
		_refreshing = false;
		applyCalls(ActiveCall::ParseList(result.toObject()));
		if (base::take(_refreshAgain)) {
			scheduleRefresh(0);
		} else {
			next();
		}
	}, [=](const HttpError &error) {
		if (!weak) {
			return;
		}
		_refreshing = false;
		next();
	});
}

void CallManager::applyCalls(const std::vector<ActiveCall> &calls) {
	applyGroupCalls(calls);
	const auto self = _backend->_selfId;
	if (_call && !_call->outgoing && _call->state == OneToOne::State::Ringing) {
		const auto i = ranges::find(calls, _call->chatId, &ActiveCall::chatId);
		const auto stillRinging = (i != end(calls))
			&& ranges::contains(i->users, i->starterId)
			&& !ranges::contains(i->users, self);
		const auto answeredElsewhere = (i != end(calls))
			&& ranges::contains(i->users, self);
		if (!stillRinging) {
			finishCall(answeredElsewhere
				? MTP_phoneCallDiscardReasonHangup()
				: MTP_phoneCallDiscardReasonMissed());
		}
	}
	const auto now = crl::now();
	for (auto i = begin(_finishedAt); i != end(_finishedAt);) {
		if (now - i->second > kIgnoreAfterFinish) {
			i = _finishedAt.erase(i);
		} else {
			++i;
		}
	}
	for (const auto &call : calls) {
		if (_finishedAt.contains(call.chatId)) {
			continue;
		} else if (call.silent
			|| call.starterId == self
			|| call.users.size() != 1
			|| call.users.front() != call.starterId
			|| (!call.sessionId.isEmpty()
				&& _seenSessions.contains(call.sessionId))) {
			continue;
		}
		if (!_call) {
			ringIncoming(call);
		}
	}
}

void CallManager::ringIncoming(const ActiveCall &call) {
	const auto chat = _backend->chatById(call.chatId);
	if (!chat) {
		const auto weak = QPointer<CallManager>(this);
		const auto chatId = call.chatId;
		_backend->loadChats({ chatId }, [=] {
			if (weak && _backend->chatById(chatId)) {
				scheduleRefresh(0);
			}
		}, [](const HttpError &) {});
		return;
	} else if (!chat->info.pm) {
		return;
	}
	const auto userId = chat->pmUserId
		? chat->pmUserId
		: _backend->otherUser(chat->info);
	if (!userId || userId != call.starterId) {
		return;
	}
	auto random = bytes::vector(MTP::ModExpFirst::kRandomPowerSize);
	base::RandomFill(bytes::make_span(random));
	auto first = MTP::CreateModExp(kDhG, DhPrime(), random);
	if (first.modexp.empty()) {
		return;
	}
	_call = std::make_unique<OneToOne>();
	_call->id = base::RandomValue<uint64>() & 0x7FFFFFFFFFFFFFFFULL;
	_call->accessHash = base::RandomValue<uint64>();
	_call->outgoing = false;
	_call->userId = userId;
	_call->chatId = call.chatId;
	_call->date = base::unixtime::now();
	_call->state = OneToOne::State::Ringing;
	_call->sessionId = call.sessionId;
	_call->randomPower = std::move(first.randomPower);
	_call->ownModExp = std::move(first.modexp);
	_call->gaHash = openssl::Sha256(_call->ownModExp);

	const auto self = _backend->_selfId;
	const auto weak = QPointer<CallManager>(this);
	const auto id = _call->id;
	_backend->ensureUsers({ userId }, [=] {
		if (!weak || !_call || _call->id != id) {
			return;
		}
		pushCall(MTP_phoneCallRequested(
			MTP_flags(0),
			MTP_long(_call->id),
			MTP_long(_call->accessHash),
			MTP_int(_call->date),
			MTP_long(_call->userId),
			MTP_long(self),
			MTP_bytes(_call->gaHash),
			protocol()));
	});
}

} // namespace Pararam
