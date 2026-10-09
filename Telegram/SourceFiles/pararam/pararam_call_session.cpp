/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_call_session.h"

#include "pararam/pararam_api.h"
#include "pararam/pararam_data.h"
#include "pararam/pararam_websocket.h"
#include "core/version.h"
#include "logs.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSysInfo>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

namespace Pararam {
namespace {

constexpr auto kApproveTimeout = crl::time(15'000);
constexpr auto kHeartbeatDelay = crl::time(5'000);
constexpr auto kHeartbeatTimeout = crl::time(8'000);
constexpr auto kRetransmitDelay = crl::time(300);
constexpr auto kMaxAttempts = 10;
constexpr auto kHttpDelay = crl::time(200);
constexpr auto kReconnectDelay = crl::time(2'000);
constexpr auto kMaxReconnects = 10;
constexpr auto kLeaveDelay = crl::time(300);
constexpr auto kSeenLimit = 512;

[[nodiscard]] bool Insecure() {
	// For a local mock media server only.
	return !qEnvironmentVariableIsEmpty("PARARAM_MEDIA_INSECURE");
}

[[nodiscard]] QString MediaUrl(const CallAccess &access, bool socket) {
	const auto scheme = socket
		? (Insecure() ? u"ws://"_q : u"wss://"_q)
		: (Insecure() ? u"http://"_q : u"https://"_q);
	return scheme + access.mediaServer;
}

[[nodiscard]] QString EscapeHtml(QString text) {
	return text.replace('&', u"&amp;"_q)
		.replace('<', u"&lt;"_q)
		.replace('>', u"&gt;"_q)
		.replace('"', u"&quot;"_q)
		.replace('\'', u"&#39;"_q);
}

[[nodiscard]] QString JsonText(const QJsonValue &value) {
	if (value.isString()) {
		return value.toString();
	} else if (value.isDouble()) {
		return QString::number(qint64(value.toDouble()));
	}
	return QString();
}

[[nodiscard]] QString PlatformName() {
#ifdef Q_OS_WIN
	return u"Windows"_q;
#elif defined Q_OS_MAC // Q_OS_WIN
	return u"macOS"_q;
#else // Q_OS_WIN || Q_OS_MAC
	return u"Linux"_q;
#endif // Q_OS_WIN || Q_OS_MAC
}

[[nodiscard]] QJsonObject RoomAuth(
		const CallAccess &access,
		int64 selfId,
		const QJsonValue &roomId) {
	auto result = QJsonObject();
	result.insert(u"user_id"_q, double(selfId));
	result.insert(u"room_id"_q, roomId);
	result.insert(u"access_key"_q, access.accessKey);
	return result;
}

[[nodiscard]] QJsonValue RoomIdValue(const QString &roomId) {
	// Send the room id back as a number if it looks like one.
	auto ok = false;
	const auto number = roomId.toLongLong(&ok);
	return ok ? QJsonValue(double(number)) : QJsonValue(roomId);
}

} // namespace

bool CallAccess::valid() const {
	return !roomId.isEmpty()
		&& !mediaServer.isEmpty()
		&& !accessKey.isEmpty();
}

CallAccess CallAccess::Parse(const QJsonObject &data) {
	auto result = CallAccess();
	result.roomId = JsonText(data.value(u"room_id"_q));
	result.mediaServer = JsonString(data, u"media_server"_q);
	result.accessKey = JsonText(data.value(u"access_key"_q));
	result.turnHost = JsonString(data, u"turnhost"_q);
	result.turnUser = JsonText(data.value(u"turnuser"_q));
	result.turnPassword = JsonText(data.value(u"turnpassword"_q));
	while (result.mediaServer.endsWith('/')) {
		result.mediaServer.chop(1);
	}
	if (result.turnHost.isEmpty()) {
		// The web client falls back to the media server host.
		result.turnHost = result.mediaServer.split('/').front();
	}
	return result;
}

std::vector<ActiveCall> ActiveCall::ParseList(const QJsonObject &data) {
	auto result = std::vector<ActiveCall>();
	for (auto i = data.begin(); i != data.end(); ++i) {
		auto ok = false;
		const auto chatId = i.key().toLongLong(&ok);
		if (!ok || !i.value().isObject()) {
			continue;
		}
		const auto object = i.value().toObject();
		auto call = ActiveCall();
		call.chatId = chatId;
		for (const auto &user : object.value(u"users"_q).toArray()) {
			const auto id = user.isObject()
				? JsonInt(user.toObject(), "id")
				: int64(user.toDouble());
			if (id) {
				call.users.push_back(id);
			}
		}
		call.starterId = JsonInt(object, u"starter_id"_q);
		call.silent = JsonBool(object, u"silent"_q);
		call.sessionId = JsonText(object.value(u"sess_id"_q));
		result.push_back(std::move(call));
	}
	return result;
}

CallUser CallUser::Parse(const QJsonObject &data) {
	auto result = CallUser();
	result.id = JsonInt(data, u"id"_q);
	result.external = (JsonString(data, u"type"_q) == u"EXTERNAL"_q);
	result.name = JsonString(data, u"name"_q);
	result.audio = JsonBool(data, u"audio"_q);
	result.video = JsonBool(data, u"video"_q);
	result.videoSource = JsonString(data, u"video_source_type"_q);
	if (result.videoSource.isEmpty()) {
		result.videoSource = result.video ? u"video"_q : u"empty"_q;
	}
	return result;
}

CallSession::CallSession(not_null<Api*> api, Options options)
: _api(api)
, _options(std::move(options))
, _retransmitTimer([=] { retransmit(); })
, _heartbeatTimer([=] { sendHeartbeat(); })
, _heartbeatTimeout([=] {
	LOG(("Pararam Call: Heartbeat timeout."));
	scheduleReconnect();
})
, _approveTimeout([=] {
	LOG(("Pararam Call: Approve timeout."));
	scheduleReconnect();
})
, _reconnectTimer([=] { requestAccess(); })
, _httpTimer([=] {
	_httpSending = false;
	sendNextHttp();
})
, _alive(std::make_shared<bool>(true)) {
}

CallSession::~CallSession() {
	*_alive = false;
	_listeners.clear();
	if (_socket) {
		_socket->closed = nullptr;
		_socket->close();
	}
}

void CallSession::start() {
	requestAccess();
}

void CallSession::requestAccess() {
	if (_finished) {
		return;
	}
	auto body = QJsonObject();
	body.insert(u"chat_id"_q, double(_options.chatId));
	const auto weak = QPointer<CallSession>(this);
	_api->post(u"/core/call"_q, body, [=](const QJsonValue &result) {
		if (!weak || _finished) {
			return;
		}
		auto access = CallAccess::Parse(result.toObject());
		if (!access.valid()) {
			finish(EndReason::Failed, u"Bad call access data."_q);
			return;
		}
		_access = std::move(access);
		connectSocket();
	}, [=](const HttpError &error) {
		if (!weak || _finished) {
			return;
		}
		if (_approved || _reconnects > 0) {
			scheduleReconnect();
		} else {
			finish(EndReason::Failed, error.message);
		}
	});
}

void CallSession::connectSocket() {
	if (_socket) {
		_socket->closed = nullptr;
		_socket->close();
		_socket = nullptr;
	}
	auto url = MediaUrl(_access, true)
		+ '/' + _access.roomId
		+ '/' + QString::number(_options.selfId)
		+ u"?access_key="_q + _access.accessKey
		+ u"&silent="_q + (_options.silent ? u"true"_q : u"false"_q)
		+ u"&stay_in_call="_q
		+ (_options.stayInCall ? u"true"_q : u"false"_q)
		+ u"&d_support=true&mixer_support=true"_q;
	if (!_callUid.isEmpty()) {
		url += u"&call_uid="_q + _callUid;
	}
	url += u"&user_name="_q + QString::fromLatin1(
		QUrl::toPercentEncoding(EscapeHtml(_options.selfName)));

	_socket = std::make_unique<WebSocket>();
	const auto socket = _socket.get();
	socket->opened = [=] { socketOpened(); };
	socket->received = [=](const QByteArray &text) {
		socketReceived(text);
	};
	socket->closed = [=](int code, const QString &reason) {
		socketClosed(code, reason);
	};
	_approveTimeout.callOnce(kApproveTimeout);
	socket->open(QUrl(url, QUrl::TolerantMode), {
		{ "Origin", "https://app.pararam.io" },
		{ "User-Agent", "Pararam Desktop/" + QByteArray(AppVersionStr) },
	});
}

void CallSession::socketOpened() {
	DEBUG_LOG(("Pararam Call: Media socket opened."));
}

void CallSession::socketClosed(int code, const QString &reason) {
	LOG(("Pararam Call: Media socket closed, code %1, reason: %2"
		).arg(code
		).arg(reason));
	if (_finished) {
		return;
	} else if (_leaving) {
		finish(EndReason::Left);
		return;
	}
	_socket = nullptr;
	if (code == 4400 || code == 4401 || code == 4403) {
		finish(EndReason::Failed, reason);
		return;
	}
	scheduleReconnect();
}

void CallSession::scheduleReconnect() {
	if (_finished || _reconnectTimer.isActive()) {
		return;
	}
	if (_leaving) {
		finish(EndReason::Left);
		return;
	}
	_approved = false;
	_heartbeatWaiting = false;
	_heartbeatTimer.cancel();
	_heartbeatTimeout.cancel();
	_approveTimeout.cancel();
	if (_socket) {
		_socket->closed = nullptr;
		_socket->close();
		_socket = nullptr;
	}
	if (++_reconnects > kMaxReconnects) {
		finish(EndReason::Failed, u"Could not reconnect."_q);
		return;
	}
	_reconnectTimer.callOnce((_reconnects == 1) ? 0 : kReconnectDelay);
}

void CallSession::socketReceived(const QByteArray &text) {
	const auto document = QJsonDocument::fromJson(text);
	if (!document.isObject()) {
		return;
	}
	const auto object = document.object();
	const auto requestId = object.value(u"request_id"_q).toString().toUtf8();
	if (!requestId.isEmpty()) {
		confirm(requestId);
		if (_seenRequestIds.contains(requestId)) {
			return;
		}
		_seenRequestIds.emplace(requestId);
		_seenOrder.push_back(requestId);
		if (_seenOrder.size() > kSeenLimit) {
			_seenRequestIds.erase(_seenOrder.front());
			_seenOrder.pop_front();
		}
	}
	const auto type = object.value(u"type"_q).toString();
	if (type == u"approve"_q) {
		const auto first = _callUid.isEmpty();
		_callUid = JsonText(object.value(u"call_uid"_q));
		_httpAuthKey = object.value(
			u"http_session_auth_key"_q).toString().toUtf8();
		_approved = true;
		_reconnects = 0;
		_approveTimeout.cancel();
		for (auto &json : base::take(_bufferedBeforeApprove)) {
			sendRaw(json);
		}
		for (auto &outgoing : _unconfirmed) {
			sendRaw(outgoing.json);
		}
		sendHeartbeat();
		if (first) {
			sendDeviceInfo();
		}
		if (!_videoQualities.empty()) {
			requestVideoQualities({});
		}
		sendNextHttp();
		if (first) {
			notify(&Listener::approved);
		}
	} else if (type == u"disconnect"_q) {
		const auto info = object.value(u"disconnectInformation"_q).toObject();
		const auto reason = JsonString(info, u"reason"_q);
		LOG(("Pararam Call: Disconnected by the server, reason: %1"
			).arg(reason));
		finish((reason == u"BANNED"_q)
			? EndReason::Banned
			: (reason == u"DROP_BY_ADMIN"_q)
			? EndReason::Dropped
			: EndReason::Left);
	} else if (type == u"error"_q) {
		const auto error = object.value(u"error"_q).toObject();
		LOG(("Pararam Call: Server error: %1"
			).arg(JsonString(error, u"message"_q)));
	} else if (type == u"hb"_q) {
		if (object.value(u"hb"_q).toInt() == _heartbeat
			&& _heartbeatWaiting) {
			_heartbeatWaiting = false;
			_heartbeatTimeout.cancel();
			_heartbeatTimer.callOnce(kHeartbeatDelay);
		}
	} else if (type == u"r-confirm-from-server"_q) {
		const auto id = object.value(u"confirm_id"_q).toString().toUtf8();
		_unconfirmed.erase(
			ranges::remove(_unconfirmed, id, &Outgoing::requestId),
			end(_unconfirmed));
		if (_unconfirmed.empty()) {
			_retransmitTimer.cancel();
		}
	} else if (type == u"message"_q) {
		handleMessage(object.value(u"message"_q).toObject());
	}
}

void CallSession::handleMessage(const QJsonObject &message) {
	const auto type = message.value(u"type"_q).toString();
	const auto data = message.value(u"data"_q).toObject();
	const auto sourceId = JsonInt(message, u"src_user_id"_q);
	if (type == u"initial"_q) {
		auto updated = std::map<int64, CallUser>();
		for (const auto &value : data.value(u"users"_q).toArray()) {
			const auto user = CallUser::Parse(value.toObject());
			if (user.id == kChoirmasterId) {
				_mediaServerJoined = true;
			} else if (user.id && user.id != _options.selfId) {
				updated.emplace(user.id, user);
			}
		}
		auto previous = base::take(_users);
		_users = updated;
		for (const auto &[id, user] : previous) {
			if (!_users.contains(id)) {
				notify(&Listener::left, id);
			}
		}
		for (const auto &[id, user] : updated) {
			if (previous.contains(id)) {
				notify(&Listener::changed, user);
			} else {
				notify(&Listener::joined, user);
			}
		}
	} else if (type == u"join"_q) {
		const auto user = CallUser::Parse(data.value(u"user"_q).toObject());
		if (user.id == kChoirmasterId) {
			_mediaServerJoined = true;
		} else if (user.id && user.id != _options.selfId) {
			const auto existed = _users.contains(user.id);
			_users[user.id] = user;
			if (existed) {
				notify(&Listener::changed, user);
			} else {
				notify(&Listener::joined, user);
			}
		}
	} else if (type == u"leave"_q) {
		if (sourceId == _options.selfId) {
			finish(EndReason::Left);
		} else if (sourceId == kChoirmasterId) {
			LOG(("Pararam Call: The media server left the call."));
			_mediaServerJoined = false;
		} else if (_users.erase(sourceId)) {
			notify(&Listener::left, sourceId);
		}
	} else if (type == u"session"_q) {
		handleSessionEvent(sourceId, data);
	}
}

void CallSession::handleSessionEvent(
		int64 sourceId,
		const QJsonObject &data) {
	const auto type = JsonString(data, u"type"_q);
	if (type == u"user-action"_q) {
		const auto action = data.value(u"userActionData"_q).toObject();
		const auto name = JsonString(action, u"action"_q);
		const auto userId = JsonInt(action, u"userId"_q)
			? JsonInt(action, u"userId"_q)
			: sourceId;
		if (name == u"USER_SPEAK_STARTED"_q || name == u"USER_SPEAKING"_q) {
			notify(&Listener::speaking, userId, true);
		} else if (name == u"USER_SPEAK_STOPPED"_q) {
			notify(&Listener::speaking, userId, false);
		}
	} else if (type == u"streamchange"_q) {
		const auto i = _users.find(sourceId);
		if (i == end(_users)) {
			return;
		}
		auto &user = i->second;
		if (data.contains(u"audio"_q)) {
			user.audio = data.value(u"audio"_q).toBool();
		}
		if (data.contains(u"video"_q)) {
			user.video = data.value(u"video"_q).toBool();
			const auto source = JsonString(data, u"video_source"_q);
			user.videoSource = !user.video
				? u"empty"_q
				: source.isEmpty()
				? u"video"_q
				: source;
		}
		const auto copy = user;
		notify(&Listener::changed, copy);
	} else if (type == u"current-video-source-user-changed"_q) {
		notify(&Listener::currentVideoUser, JsonInt(data, u"user_id"_q));
	}
}

void CallSession::sendHeartbeat() {
	if (!_socket || !_socket->isOpen()) {
		return;
	}
	++_heartbeat;
	auto object = QJsonObject();
	object.insert(u"type"_q, u"hb"_q);
	object.insert(u"hb"_q, _heartbeat);
	sendRaw(QJsonDocument(object).toJson(QJsonDocument::Compact));
	_heartbeatWaiting = true;
	_heartbeatTimeout.callOnce(kHeartbeatTimeout);
}

void CallSession::sendReliable(QJsonObject message) {
	const auto json = QJsonDocument(message).toJson(QJsonDocument::Compact);
	const auto requestId = QCryptographicHash::hash(
		json,
		QCryptographicHash::Md5).toHex().mid(0, 8)
		+ '_'
		+ QByteArray::number(QDateTime::currentMSecsSinceEpoch());
	const auto rest = json.mid(1);
	const auto full = "{\"request_id\":\"" + requestId + "\""
		+ ((rest == "}") ? QByteArray() : QByteArray(","))
		+ rest;
	if (!_approved) {
		_bufferedBeforeApprove.push_back(full);
		return;
	}
	_unconfirmed.push_back({ requestId, full, 1 });
	sendRaw(full);
	if (!_retransmitTimer.isActive()) {
		_retransmitTimer.callEach(kRetransmitDelay);
	}
}

void CallSession::retransmit() {
	if (!_approved) {
		return;
	}
	for (auto i = begin(_unconfirmed); i != end(_unconfirmed);) {
		if (i->attempts >= kMaxAttempts) {
			LOG(("Pararam Call: Message was not confirmed: %1"
				).arg(QString::fromUtf8(i->json)));
			i = _unconfirmed.erase(i);
		} else {
			++i->attempts;
			sendRaw(i->json);
			++i;
		}
	}
	if (_unconfirmed.empty()) {
		_retransmitTimer.cancel();
	}
}

void CallSession::confirm(const QByteArray &requestId) {
	sendRaw("{\"type\":\"r-confirm-from-client\",\"confirm_id\":\""
		+ requestId
		+ "\"}");
}

void CallSession::sendRaw(const QByteArray &json) {
	if (_socket && _socket->isOpen()) {
		_socket->send(json);
	}
}

void CallSession::queueHttp(HttpRequest request) {
	_http.push_back(std::move(request));
	sendNextHttp();
}

void CallSession::sendNextHttp() {
	if (_httpSending || _http.empty() || !_approved || _finished) {
		return;
	}
	auto request = std::move(_http.front());
	_http.pop_front();
	_httpSending = true;

	const auto weak = QPointer<CallSession>(this);
	const auto done = request.done;
	const auto fail = request.fail;
	_api->postAbsolute(
		QUrl(MediaUrl(_access, false) + request.path),
		request.body,
		{ { "XCALL-SESSION-AUTH-KEY", _httpAuthKey } },
		[=](const QJsonValue &result) {
			if (!weak) {
				return;
			}
			_httpTimer.callOnce(kHttpDelay);
			auto data = result.toObject().value(u"data"_q);
			if (data.isString()) {
				const auto document = QJsonDocument::fromJson(
					data.toString().toUtf8());
				data = document.isObject()
					? QJsonValue(document.object())
					: data;
			}
			if (done) {
				done(data);
			}
		},
		[=](const HttpError &error) {
			if (!weak) {
				return;
			}
			_httpTimer.callOnce(kHttpDelay);
			LOG(("Pararam Call: Media server request failed: %1 %2"
				).arg(error.status
				).arg(error.message));
			if (fail) {
				fail(error.message.isEmpty()
					? u"Request failed."_q
					: error.message);
			}
		});
}

void CallSession::sendDeviceInfo() {
	auto body = QJsonObject();
	body.insert(u"app_type"_q, u"browser"_q);
	body.insert(u"browser"_q, u"Pararam Desktop"_q);
	body.insert(u"browser_ver"_q, QString::fromLatin1(AppVersionStr));
	body.insert(u"os"_q, PlatformName());
	body.insert(u"os_ver"_q, QSysInfo::productVersion());
	body.insert(u"app_ver"_q, QString::fromLatin1(AppVersionStr));
	queueHttp({ u"/session/device-info"_q, body });
}

void CallSession::sendOffer(
		int64 dstUserId,
		const QByteArray &sdp,
		bool iceRestart,
		std::function<void(QByteArray answer)> done,
		std::function<void(QString error)> fail) {
	auto description = QJsonObject();
	description.insert(u"type"_q, u"offer"_q);
	description.insert(u"sdp"_q, QString::fromUtf8(sdp));
	auto body = QJsonObject();
	body.insert(u"sdp"_q, description);
	body.insert(
		u"timestamp"_q,
		double(QDateTime::currentMSecsSinceEpoch()));
	body.insert(u"dst_user_id"_q, double(dstUserId));
	body.insert(u"ice_restart"_q, iceRestart);
	queueHttp({ u"/session/sdp-offer"_q, body, [=](const QJsonValue &data) {
		const auto object = data.toObject();
		auto answer = JsonString(object, u"sdpAnswer"_q);
		if (answer.isEmpty()) {
			answer = JsonString(object, u"sdp"_q);
		}
		if (answer.isEmpty()) {
			fail(u"No SDP answer."_q);
		} else {
			done(answer.toUtf8());
		}
	}, fail });
}

void CallSession::setAudioEnabled(bool enabled) {
	auto body = QJsonObject();
	body.insert(u"audio"_q, enabled);
	queueHttp({ u"/session/stream-change"_q, body });
}

void CallSession::setVideoEnabled(bool enabled, const QString &source) {
	auto body = QJsonObject();
	body.insert(u"video"_q, enabled);
	if (enabled) {
		body.insert(u"video_source"_q, source);
	}
	queueHttp({ u"/session/stream-change"_q, body });
}

void CallSession::requestVideoQualities(
		const std::map<int64, QString> &qualities) {
	for (const auto &[id, quality] : qualities) {
		_videoQualities[id] = quality;
	}
	if (!_approved || _videoQualities.empty()) {
		return;
	}
	auto dict = QJsonObject();
	for (const auto &[id, quality] : _videoQualities) {
		dict.insert(QString::number(id), quality);
	}
	auto data = QJsonObject();
	data.insert(u"type"_q, u"vqualities-dict"_q);
	data.insert(u"vq_dict"_q, dict);
	auto message = QJsonObject();
	message.insert(u"type"_q, u"session"_q);
	message.insert(u"dst_user_id"_q, double(kChoirmasterId));
	message.insert(u"data"_q, data);
	auto envelope = QJsonObject();
	envelope.insert(u"type"_q, u"message"_q);
	envelope.insert(u"message"_q, message);
	sendReliable(envelope);
}

void CallSession::leave() {
	if (_finished || _leaving) {
		return;
	}
	_leaving = true;
	if (_socket && _socket->isOpen() && _approved) {
		auto message = QJsonObject();
		message.insert(u"type"_q, u"leave"_q);
		sendReliable(message);
		const auto weak = QPointer<CallSession>(this);
		QTimer::singleShot(kLeaveDelay, this, [=] {
			if (weak) {
				finish(EndReason::Left);
			}
		});
	} else {
		if (_access.valid()) {
			_api->postAbsolute(
				QUrl(MediaUrl(_access, false) + u"/leave"_q),
				RoomAuth(_access, _options.selfId, RoomIdValue(_access.roomId)),
				{},
				[](const QJsonValue &) {},
				[](const HttpError &) {});
		}
		finish(EndReason::Left);
	}
}

void CallSession::Reject(not_null<Api*> api, int64 chatId, int64 selfId) {
	auto body = QJsonObject();
	body.insert(u"chat_id"_q, double(chatId));
	const auto weak = QPointer<Api>(api.get());
	api->post(u"/core/call"_q, body, [=](const QJsonValue &result) {
		const auto access = CallAccess::Parse(result.toObject());
		if (!weak || !access.valid()) {
			return;
		}
		weak->postAbsolute(
			QUrl(MediaUrl(access, false) + u"/reject"_q),
			RoomAuth(access, selfId, RoomIdValue(access.roomId)),
			{},
			[](const QJsonValue &) {},
			[](const HttpError &error) {
				LOG(("Pararam Call: Reject failed: %1"
					).arg(error.message));
			});
	}, [](const HttpError &error) {
		LOG(("Pararam Call: Could not get access to reject: %1"
			).arg(error.message));
	});
}

void CallSession::finish(EndReason reason, const QString &error) {
	if (_finished) {
		return;
	}
	_finished = true;
	_approved = false;
	_retransmitTimer.cancel();
	_heartbeatTimer.cancel();
	_heartbeatTimeout.cancel();
	_approveTimeout.cancel();
	_reconnectTimer.cancel();
	_httpTimer.cancel();
	_http.clear();
	if (_socket) {
		_socket->closed = nullptr;
		_socket->close();
	}
	if (!error.isEmpty()) {
		LOG(("Pararam Call: Finished with error: %1").arg(error));
	}
	notify(&Listener::ended, reason, error);
}

template <typename Method, typename ...Args>
void CallSession::notify(Method method, const Args &...args) {
	const auto alive = _alive;
	auto listeners = std::vector<Listener>();
	for (const auto &[id, listener] : _listeners) {
		listeners.push_back(listener);
	}
	for (const auto &listener : listeners) {
		if (!*alive) {
			return;
		}
		if (const auto &callback = listener.*method) {
			callback(args...);
		}
	}
}

int CallSession::addListener(Listener listener) {
	_listeners.emplace(++_listenerId, std::move(listener));
	return _listenerId;
}

void CallSession::removeListener(int id) {
	_listeners.erase(id);
}

int64 CallSession::chatId() const {
	return _options.chatId;
}

int64 CallSession::selfId() const {
	return _options.selfId;
}

bool CallSession::silent() const {
	return _options.silent;
}

bool CallSession::approved() const {
	return _approved;
}

bool CallSession::finished() const {
	return _finished;
}

const CallAccess &CallSession::access() const {
	return _access;
}

const std::map<int64, CallUser> &CallSession::users() const {
	return _users;
}

bool CallSession::mediaServerJoined() const {
	return _mediaServerJoined;
}

} // namespace Pararam
